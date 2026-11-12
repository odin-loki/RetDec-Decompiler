#include "fuse/ui/widgets/widgets.hpp"
#include "widgets_internal.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace fuse::ui::widgets {
namespace {

constexpr f32 kPi = 3.14159265f;

f32 lineHeight(UiContext& ui, f32 px) { return ui.font() ? std::ceil(ui.font()->metrics(px).lineHeight()) : std::ceil(px * 1.25f); }

Size textSize(UiContext& ui, NodeHandle n, std::string_view s) {
    const f32 px = ui.computed(n).fontSize;
    return {std::ceil(ui.measureString(s, px)), lineHeight(ui, px)};
}

Color accent(const PaintContext& pc) { return pc.tint(pc.style.background2.a > 0.f ? pc.style.background2 : Color::rgb(0x3D8BFD)); }
Color textCol(const PaintContext& pc) { return pc.tint(pc.style.color); }

void labelAt(const PaintContext& pc, DrawList& dl, std::string_view s, Rect box, TextAlignment al) {
    pc.ui.paintString(dl, s, box, textCol(pc), pc.style.fontSize, al, pc.node.index);
}

// value <-> fraction helpers
f32 frac(UiContext& ui, NodeHandle n) {
    const f32 lo = ui.rangeMin(n), hi = ui.rangeMax(n);
    return hi > lo ? std::clamp((ui.value(n) - lo) / (hi - lo), 0.f, 1.f) : 0.f;
}
void setFrac(UiContext& ui, NodeHandle n, f32 f) {
    const f32 lo = ui.rangeMin(n), hi = ui.rangeMax(n);
    ui.setValue(n, lo + std::clamp(f, 0.f, 1.f) * (hi - lo), true);
}
f32 stepOf(UiContext& ui, NodeHandle n) {
    const f32 s = ui.rangeStep(n);
    return s > 0.f ? s : (ui.rangeMax(n) - ui.rangeMin(n)) / 20.f;
}
bool rangeKeys(EventContext& ec, const Event& e, bool vertical) {
    if (e.type != EventType::KeyDown || ec.phase != Phase::Target) return false;
    UiContext& ui = ec.ui;
    const f32 st = stepOf(ui, ec.node);
    const Key inc = vertical ? Key::Up : Key::Right, dec = vertical ? Key::Down : Key::Left;
    if (e.key == inc || e.key == Key::PageUp) { ui.setValue(ec.node, ui.value(ec.node) + (e.key == Key::PageUp ? st * 10.f : st), true); return true; }
    if (e.key == dec || e.key == Key::PageDown) { ui.setValue(ec.node, ui.value(ec.node) - (e.key == Key::PageDown ? st * 10.f : st), true); return true; }
    if (e.key == Key::Home) { ui.setValue(ec.node, ui.rangeMin(ec.node), true); return true; }
    if (e.key == Key::End) { ui.setValue(ec.node, ui.rangeMax(ec.node), true); return true; }
    return false;
}

// ---- Label -------------------------------------------------------------------------------------------------------
// (core "label" class: text measured and painted by the core)

// ---- Button / ToolButton / Link ------------------------------------------------------------------------------------
struct Button {
    static constexpr std::string_view kName = "button";
    static constexpr A11yRole kRole = A11yRole::Button;
    static constexpr bool kFocusable = true;
    static constexpr bool kInteractive = true;
    static Size measure(UiContext& ui, NodeHandle n, Size) { return textSize(ui, n, ui.text(n)); }
    static void paint(const PaintContext& pc, DrawList& dl) { labelAt(pc, dl, pc.ui.text(pc.node), pc.content, TextAlignment::Center); }
};
struct ToolButton : Button { static constexpr std::string_view kName = "toolbutton"; };
struct Link {
    static constexpr std::string_view kName = "link";
    static constexpr A11yRole kRole = A11yRole::Link;
    static constexpr bool kFocusable = true;
    static constexpr bool kInteractive = true;
    static Size measure(UiContext& ui, NodeHandle n, Size) { return textSize(ui, n, ui.text(n)); }
    static void paint(const PaintContext& pc, DrawList& dl) {
        labelAt(pc, dl, pc.ui.text(pc.node), pc.content, TextAlignment::Start);
        const f32 w = pc.ui.measureString(pc.ui.text(pc.node), pc.style.fontSize);
        const f32 y = pc.content.bottom() - 1.f;
        if (pc.states & (kStateHover | kStateFocusVisible)) dl.line({pc.content.x, y}, {pc.content.x + w, y}, textCol(pc), 1.f);
    }
};

// ---- CheckBox / RadioButton / Switch -------------------------------------------------------------------------------
Size indicatorMeasure(UiContext& ui, NodeHandle n, f32 indicatorW) {
    const Size t = textSize(ui, n, ui.text(n));
    const f32 h = std::max(t.h, 16.f);
    return {indicatorW + (t.w > 0.f ? 6.f + t.w : 0.f), h};
}
struct CheckBox {
    static constexpr std::string_view kName = "checkbox";
    static constexpr A11yRole kRole = A11yRole::CheckBox;
    static constexpr bool kFocusable = true;
    static constexpr bool kInteractive = true;
    static Size measure(UiContext& ui, NodeHandle n, Size) { return indicatorMeasure(ui, n, 16.f); }
    static void paint(const PaintContext& pc, DrawList& dl) {
        const Rect b{pc.content.x, std::round(pc.content.y + (pc.content.h - 16.f) * 0.5f), 16.f, 16.f};
        const bool on = pc.states & kStateChecked;
        dl.roundedRect(b, on ? accent(pc) : pc.tint(Color::rgb(0x000000, 0.25f)), {3, 3, 3, 3}, pc.tint(pc.style.borderColor.a > 0 ? pc.style.borderColor : Color::rgb(0x808080)), on ? 0.f : 1.f);
        if (on) {
            const Color w = pc.tint(Color::rgb(0xFFFFFF));
            dl.line({b.x + 3.5f, b.y + 8.5f}, {b.x + 6.5f, b.y + 11.5f}, w, 2.f);
            dl.line({b.x + 6.5f, b.y + 11.5f}, {b.x + 12.5f, b.y + 4.5f}, w, 2.f);
        }
        labelAt(pc, dl, pc.ui.text(pc.node), {b.right() + 6.f, pc.content.y, pc.content.w - 22.f, pc.content.h}, TextAlignment::Start);
    }
    static bool onEvent(EventContext& ec, const Event& e) {
        if (e.type == EventType::Click && ec.phase == Phase::Target) {
            ec.ui.setChecked(ec.node, !ec.ui.checked(ec.node));
            ec.ui.emit(ec.node, SignalKind::Toggled, {.flag = ec.ui.checked(ec.node)});
        }
        return false;
    }
};
struct RadioButton {
    static constexpr std::string_view kName = "radio";
    static constexpr A11yRole kRole = A11yRole::RadioButton;
    static constexpr bool kFocusable = true;
    static constexpr bool kInteractive = true;
    static Size measure(UiContext& ui, NodeHandle n, Size) { return indicatorMeasure(ui, n, 16.f); }
    static void paint(const PaintContext& pc, DrawList& dl) {
        const Rect b{pc.content.x, std::round(pc.content.y + (pc.content.h - 16.f) * 0.5f), 16.f, 16.f};
        const bool on = pc.states & kStateChecked;
        dl.roundedRect(b, pc.tint(Color::rgb(0x000000, 0.25f)), {8, 8, 8, 8}, on ? accent(pc) : pc.tint(Color::rgb(0x808080)), on ? 2.f : 1.f);
        if (on) dl.rect({b.x + 4.f, b.y + 4.f, 8.f, 8.f}, accent(pc), 4.f);
        labelAt(pc, dl, pc.ui.text(pc.node), {b.right() + 6.f, pc.content.y, pc.content.w - 22.f, pc.content.h}, TextAlignment::Start);
    }
    static bool onEvent(EventContext& ec, const Event& e) {
        if (e.type == EventType::Click && ec.phase == Phase::Target && !ec.ui.checked(ec.node)) {
            const u32 cls = ec.ui.classOf(ec.node);
            for (NodeHandle s = ec.ui.firstChild(ec.ui.parent(ec.node)); s.valid(); s = ec.ui.nextSibling(s))
                if (s != ec.node && ec.ui.classOf(s) == cls && ec.ui.checked(s)) {
                    ec.ui.setChecked(s, false);
                    ec.ui.emit(s, SignalKind::Toggled, {.flag = false});
                }
            ec.ui.setChecked(ec.node, true);
            ec.ui.emit(ec.node, SignalKind::Toggled, {.flag = true});
        }
        return false;
    }
};
struct Switch : CheckBox {
    static constexpr std::string_view kName = "switch";
    static constexpr A11yRole kRole = A11yRole::Switch;
    static Size measure(UiContext& ui, NodeHandle n, Size) { return indicatorMeasure(ui, n, 32.f); }
    static void paint(const PaintContext& pc, DrawList& dl) {
        const Rect t{pc.content.x, std::round(pc.content.y + (pc.content.h - 18.f) * 0.5f), 32.f, 18.f};
        const bool on = pc.states & kStateChecked;
        dl.rect(t, on ? accent(pc) : pc.tint(Color::rgb(0x5A5A66)), 9.f);
        dl.rect({on ? t.right() - 16.f : t.x + 2.f, t.y + 2.f, 14.f, 14.f}, pc.tint(Color::rgb(0xFFFFFF)), 7.f);
        labelAt(pc, dl, pc.ui.text(pc.node), {t.right() + 6.f, pc.content.y, pc.content.w - 38.f, pc.content.h}, TextAlignment::Start);
    }
};

// ---- Slider / ScrollBar / Dial / ProgressBar -----------------------------------------------------------------------
struct Slider {
    static constexpr std::string_view kName = "slider";
    static constexpr A11yRole kRole = A11yRole::Slider;
    static constexpr bool kFocusable = true;
    static constexpr bool kInteractive = true;
    static Size measure(UiContext&, NodeHandle, Size) { return {120.f, 18.f}; }
    static void paint(const PaintContext& pc, DrawList& dl) {
        const Rect c = pc.content;
        const f32 f = frac(pc.ui, pc.node);
        const f32 cy = std::round(c.y + c.h * 0.5f);
        const Rect track{c.x + 7.f, cy - 2.f, std::max(0.f, c.w - 14.f), 4.f};
        dl.rect(track, pc.tint(Color::rgb(0x5A5A66)), 2.f);
        dl.rect({track.x, track.y, track.w * f, 4.f}, accent(pc), 2.f);
        const f32 kx = std::round(track.x + track.w * f);
        dl.roundedRect({kx - 7.f, cy - 7.f, 14.f, 14.f}, pc.tint(Color::rgb(0xF0F0F5)), {7, 7, 7, 7}, accent(pc), (pc.states & kStateFocusVisible) ? 2.f : 0.f);
    }
    static bool onEvent(EventContext& ec, const Event& e) {
        if (ec.phase != Phase::Target) return false;
        if (e.type == EventType::PointerDown || (e.type == EventType::PointerMove && ec.ui.pressed() == ec.node)) {
            const Rect c = ec.ui.contentRect(ec.node);
            setFrac(ec.ui, ec.node, (e.pos.x - c.x - 7.f) / std::max(1.f, c.w - 14.f));
            return true;
        }
        return rangeKeys(ec, e, false);
    }
};
struct ScrollBar {
    static constexpr std::string_view kName = "scrollbar";
    static constexpr A11yRole kRole = A11yRole::ScrollBar;
    static constexpr bool kInteractive = true;
    static Size measure(UiContext&, NodeHandle, Size) { return {12.f, 12.f}; }
    static bool vertical(const Rect& r) { return r.h >= r.w; }
    // page fraction (thumb length / track) is stored in the node's range step when > 0 and < 1, otherwise 0.2
    static f32 page(UiContext& ui, NodeHandle n) { const f32 p = ui.rangeStep(n) / std::max(1e-6f, ui.rangeMax(n) - ui.rangeMin(n)); return p > 0.f && p < 1.f ? p : 0.2f; }
    static void paint(const PaintContext& pc, DrawList& dl) {
        const Rect c = pc.content;
        const bool v = vertical(c);
        const f32 len = v ? c.h : c.w, pg = page(pc.ui, pc.node), th = std::max(16.f, len * pg);
        const f32 at = (len - th) * frac(pc.ui, pc.node);
        dl.rect(c, pc.tint(Color::rgb(0x000000, 0.2f)), 4.f);
        const Rect t = v ? Rect{c.x + 2.f, c.y + at, c.w - 4.f, th} : Rect{c.x + at, c.y + 2.f, th, c.h - 4.f};
        dl.rect(t, pc.states & (kStateHover | kStatePressed) ? accent(pc) : pc.tint(Color::rgb(0x8A8A96)), 3.f);
    }
    static bool onEvent(EventContext& ec, const Event& e) {
        if (ec.phase != Phase::Target) return false;
        if (e.type == EventType::PointerDown || (e.type == EventType::PointerMove && ec.ui.pressed() == ec.node)) {
            const Rect c = ec.ui.contentRect(ec.node);
            const bool v = vertical(c);
            const f32 len = v ? c.h : c.w, th = std::max(16.f, len * page(ec.ui, ec.node));
            const f32 p = v ? e.pos.y - c.y : e.pos.x - c.x;
            setFrac(ec.ui, ec.node, (p - th * 0.5f) / std::max(1.f, len - th));
            return true;
        }
        if (e.type == EventType::Wheel) {
            const f32 lo = ec.ui.rangeMin(ec.node), hi = ec.ui.rangeMax(ec.node);
            const f32 n = e.wheel.y != 0.f ? e.wheel.y : e.wheel.x;
            ec.ui.setValue(ec.node, ec.ui.value(ec.node) - n * 0.1f * (hi - lo), true); // one notch = 10% of the range
            return true;
        }
        return false;
    }
};
struct Dial {
    static constexpr std::string_view kName = "dial";
    static constexpr A11yRole kRole = A11yRole::Dial;
    static constexpr bool kFocusable = true;
    static constexpr bool kInteractive = true;
    static Size measure(UiContext&, NodeHandle, Size) { return {40.f, 40.f}; }
    static void paint(const PaintContext& pc, DrawList& dl) {
        const Rect c = pc.content;
        const f32 d = std::min(c.w, c.h), r = d * 0.5f;
        const Vec2 o{c.x + c.w * 0.5f, c.y + c.h * 0.5f};
        dl.roundedRect({o.x - r, o.y - r, d, d}, pc.tint(Color::rgb(0x2A2A33)), {r, r, r, r}, accent(pc), 2.f);
        const f32 a = (-225.f + 270.f * frac(pc.ui, pc.node)) * kPi / 180.f;
        dl.line(o, {o.x + std::cos(a) * (r - 4.f), o.y + std::sin(a) * (r - 4.f)}, textCol(pc), 2.f);
    }
    static bool onEvent(EventContext& ec, const Event& e) {
        if (ec.phase != Phase::Target) return false;
        if (e.type == EventType::PointerDown || (e.type == EventType::PointerMove && ec.ui.pressed() == ec.node)) {
            const Rect c = ec.ui.contentRect(ec.node);
            const f32 ang = std::atan2(e.pos.y - (c.y + c.h * 0.5f), e.pos.x - (c.x + c.w * 0.5f)) * 180.f / kPi;
            f32 deg = ang + 225.f; // 0 at the start of the 270-degree sweep
            while (deg < 0.f) deg += 360.f;
            while (deg >= 360.f) deg -= 360.f;
            if (deg <= 270.f) setFrac(ec.ui, ec.node, deg / 270.f);
            return true;
        }
        return rangeKeys(ec, e, true) || rangeKeys(ec, e, false);
    }
};
struct ProgressBar {
    static constexpr std::string_view kName = "progressbar";
    static constexpr A11yRole kRole = A11yRole::ProgressBar;
    static Size measure(UiContext&, NodeHandle, Size) { return {120.f, 8.f}; }
    static void paint(const PaintContext& pc, DrawList& dl) {
        const f32 r = std::min(pc.content.h * 0.5f, pc.style.radius[0]);
        dl.rect(pc.content, pc.tint(Color::rgb(0x000000, 0.3f)), r);
        const f32 w = std::round(pc.content.w * frac(pc.ui, pc.node));
        if (w > 0.f) dl.rect({pc.content.x, pc.content.y, w, pc.content.h}, accent(pc), r);
    }
};

// ---- SpinBox / ComboBox --------------------------------------------------------------------------------------------
struct SpinBox {
    static constexpr std::string_view kName = "spinbox";
    static constexpr A11yRole kRole = A11yRole::SpinBox;
    static constexpr bool kFocusable = true;
    static constexpr bool kInteractive = true;
    static int fmt(UiContext& ui, NodeHandle n, char* buf, int cap) {
        const f32 st = ui.rangeStep(n);
        const int dec = st > 0.f && st < 1.f ? (st < 0.1f ? 2 : 1) : 0;
        return std::snprintf(buf, std::size_t(cap), "%.*f", dec, double(ui.value(n)));
    }
    static Size measure(UiContext& ui, NodeHandle n, Size) {
        char buf[32];
        const int len = fmt(ui, n, buf, 32);
        const Size t = textSize(ui, n, std::string_view(buf, std::size_t(std::max(0, len))));
        return {std::max(48.f, t.w) + 20.f, t.h};
    }
    static void paint(const PaintContext& pc, DrawList& dl) {
        char buf[32];
        const int len = fmt(pc.ui, pc.node, buf, 32);
        const Rect c = pc.content;
        labelAt(pc, dl, std::string_view(buf, std::size_t(std::max(0, len))), {c.x, c.y, c.w - 20.f, c.h}, TextAlignment::Start);
        const f32 ax = c.right() - 10.f, my = c.y + c.h * 0.5f;
        const Color a = textCol(pc);
        dl.line({ax - 4.f, my - 3.f}, {ax, my - 7.f}, a, 1.5f);
        dl.line({ax, my - 7.f}, {ax + 4.f, my - 3.f}, a, 1.5f);
        dl.line({ax - 4.f, my + 3.f}, {ax, my + 7.f}, a, 1.5f);
        dl.line({ax, my + 7.f}, {ax + 4.f, my + 3.f}, a, 1.5f);
    }
    static bool onEvent(EventContext& ec, const Event& e) {
        if (ec.phase != Phase::Target) return false;
        UiContext& ui = ec.ui;
        const f32 st = ui.rangeStep(ec.node) > 0.f ? ui.rangeStep(ec.node) : 1.f;
        if (e.type == EventType::PointerDown) {
            const Rect c = ui.contentRect(ec.node);
            if (e.pos.x >= c.right() - 20.f) ui.setValue(ec.node, ui.value(ec.node) + (e.pos.y < c.y + c.h * 0.5f ? st : -st), true);
            return true;
        }
        if (e.type == EventType::Wheel) { ui.setValue(ec.node, ui.value(ec.node) + (e.wheel.y > 0.f ? st : -st), true); return true; }
        if (e.type == EventType::KeyDown && (e.key == Key::Up || e.key == Key::Down)) {
            ui.setValue(ec.node, ui.value(ec.node) + (e.key == Key::Up ? st : -st), true);
            return true;
        }
        return rangeKeys(ec, e, true);
    }
};
std::string_view item(std::string_view items, u32 index) {
    u32 i = 0;
    std::size_t at = 0;
    while (true) {
        const std::size_t nl = items.find('\n', at);
        if (i == index) return items.substr(at, nl == std::string_view::npos ? std::string_view::npos : nl - at);
        if (nl == std::string_view::npos) return {};
        at = nl + 1;
        ++i;
    }
}
u32 itemCount(std::string_view items) { return items.empty() ? 0u : u32(std::count(items.begin(), items.end(), '\n') + 1); }
struct ComboBox {
    static constexpr std::string_view kName = "combobox";
    static constexpr A11yRole kRole = A11yRole::ComboBox;
    static constexpr bool kFocusable = true;
    static constexpr bool kInteractive = true;
    static Size measure(UiContext& ui, NodeHandle n, Size) {
        f32 w = 0.f;
        const std::string_view items = ui.text(n);
        for (u32 k = 0, c = itemCount(items); k < c; ++k) w = std::max(w, textSize(ui, n, item(items, k)).w);
        return {w + 22.f, lineHeight(ui, ui.computed(n).fontSize)};
    }
    static void paint(const PaintContext& pc, DrawList& dl) {
        const Rect c = pc.content;
        labelAt(pc, dl, item(pc.ui.text(pc.node), u32(pc.ui.value(pc.node))), {c.x, c.y, c.w - 18.f, c.h}, TextAlignment::Start);
        const f32 ax = c.right() - 8.f, my = c.y + c.h * 0.5f;
        dl.line({ax - 4.f, my - 2.f}, {ax, my + 2.f}, textCol(pc), 1.5f);
        dl.line({ax, my + 2.f}, {ax + 4.f, my - 2.f}, textCol(pc), 1.5f);
    }
    static bool onEvent(EventContext& ec, const Event& e) {
        if (ec.phase != Phase::Target) return false;
        UiContext& ui = ec.ui;
        const u32 count = itemCount(ui.text(ec.node));
        if (!count) return false;
        const u32 cur = u32(ui.value(ec.node));
        auto pick = [&](u32 v) { ui.setValue(ec.node, f32(v), true); ui.emit(ec.node, SignalKind::TextChanged, {.text = item(ui.text(ec.node), v)}); };
        if (e.type == EventType::Click) {
            if (detail::openComboPopup(ui, ec.node)) return true; // Tier B popup list
            pick((cur + 1) % count);
            return true;
        }
        if (e.type == EventType::KeyDown && e.key == Key::Down) { pick(std::min(cur + 1, count - 1)); return true; }
        if (e.type == EventType::KeyDown && e.key == Key::Up) { pick(cur ? cur - 1 : 0); return true; }
        if (e.type == EventType::Wheel) { pick(e.wheel.y < 0.f ? std::min(cur + 1, count - 1) : (cur ? cur - 1 : 0)); return true; }
        return false;
    }
};

// ---- LineEdit ------------------------------------------------------------------------------------------------------
struct LineEdit {
    static constexpr std::string_view kName = "lineedit";
    static constexpr A11yRole kRole = A11yRole::TextField;
    static constexpr bool kFocusable = true;
    static constexpr bool kInteractive = true;
    static Size measure(UiContext& ui, NodeHandle n, Size) { return {160.f, lineHeight(ui, ui.computed(n).fontSize)}; }
    static void paint(const PaintContext& pc, DrawList& dl) {
        UiContext& ui = pc.ui;
        const Rect c = pc.content;
        auto& eb = ui.editBuffer(pc.node);
        const std::string_view t = eb.text();
        const f32 px = pc.style.fontSize;
        const bool focused = pc.states & kStateFocus;
        // IME pre-edit (UI1) is shown inline at the cursor, underlined, with its own caret; it is not in the buffer.
        const std::string_view pre = focused ? ui.composition() : std::string_view{};
        const f32 cx = ui.measureString(t.substr(0, eb.cursor()), px);
        const f32 preW = pre.empty() ? 0.f : ui.measureString(pre, px);
        const f32 caretX = cx + (pre.empty() ? 0.f : ui.measureString(pre.substr(0, std::size_t(ui.compositionCaret())), px));
        const f32 scrollX = std::max(0.f, caretX - c.w + 2.f);
        dl.pushClip(c);
        if (focused && eb.hasSelection() && pre.empty()) {
            const std::size_t a = std::min(eb.cursor(), eb.anchor()), b = std::max(eb.cursor(), eb.anchor());
            const f32 x0 = ui.measureString(t.substr(0, a), px), x1 = ui.measureString(t.substr(0, b), px);
            dl.rect({c.x + x0 - scrollX, c.y, x1 - x0, c.h}, accent(pc).withAlpha(0.45f * pc.opacity));
        }
        if (pre.empty()) {
            labelAt(pc, dl, t, {c.x - scrollX, c.y, c.w + scrollX + 1000.f, c.h}, TextAlignment::Start);
        } else {
            const f32 x = c.x - scrollX;
            labelAt(pc, dl, t.substr(0, eb.cursor()), {x, c.y, cx + 1000.f, c.h}, TextAlignment::Start);
            labelAt(pc, dl, pre, {x + cx, c.y, preW + 1000.f, c.h}, TextAlignment::Start);
            labelAt(pc, dl, t.substr(eb.cursor()), {x + cx + preW, c.y, c.w + 1000.f, c.h}, TextAlignment::Start);
            dl.rect({std::round(x + cx), std::round(c.y + c.h - 3.f), preW, 1.f}, textCol(pc));
        }
        if (focused) {
            const Rect caret{std::round(c.x + caretX - scrollX), c.y + 2.f, 1.5f, c.h - 4.f};
            dl.rect(caret, textCol(pc));
            ui.setTextCaret(pc.node, caret);
        }
        dl.popClip();
    }
    static void changed(UiContext& ui, NodeHandle n) {
        const std::string& t = ui.editBuffer(n).text();
        ui.setText(n, t);
        ui.emit(n, SignalKind::TextChanged, {.text = t});
    }
    static bool onEvent(EventContext& ec, const Event& e) {
        if (ec.phase != Phase::Target) return false;
        UiContext& ui = ec.ui;
        auto& eb = ui.editBuffer(ec.node);
        if (e.type == EventType::Text) { eb.insert(e.text); changed(ui, ec.node); return true; }
        if (e.type == EventType::Composition) { ui.setValue(ec.node, ui.value(ec.node) + 1.f); return true; } // repaint
        if (e.type == EventType::PointerDown) {
            const Rect c = ui.contentRect(ec.node);
            const std::string& t = eb.text();
            eb.home(false);
            const f32 px = ui.computed(ec.node).fontSize;
            while (eb.cursor() < t.size()) {
                const std::size_t nx = fuse::text::nextBoundary(t, eb.cursor());
                const f32 mid = (ui.measureString(std::string_view(t).substr(0, eb.cursor()), px) + ui.measureString(std::string_view(t).substr(0, nx), px)) * 0.5f;
                if (c.x + mid > e.pos.x) break;
                eb.moveRight(false);
            }
            ui.setValue(ec.node, ui.value(ec.node) + 1.f); // caret moved: repaint
            return true;
        }
        if (e.type != EventType::KeyDown) return false;
        const bool sh = e.mods & kModShift, ctrl = e.mods & kModCtrl;
        bool edit = false;
        switch (e.key) {
        case Key::Left: ctrl ? eb.wordLeft(sh) : eb.moveLeft(sh); break;
        case Key::Right: ctrl ? eb.wordRight(sh) : eb.moveRight(sh); break;
        case Key::Home: eb.home(sh); break;
        case Key::End: eb.end(sh); break;
        case Key::Backspace: eb.backspace(); edit = true; break;
        case Key::Delete: eb.del(); edit = true; break;
        case Key::A: if (!ctrl) return false; eb.selectAll(); break;
        case Key::C:
        case Key::X:
            if (!ctrl) return false;
            if (!eb.hasSelection()) return true;
            ui.setClipboardText(eb.selection());
            if (e.key == Key::X) { eb.insert({}); edit = true; }
            break;
        case Key::V: {
            if (!ctrl) return false;
            std::string paste;
            if (!ui.clipboardText(paste)) return true;
            // single line: newlines become spaces
            for (char& ch : paste) if (ch == '\n' || ch == '\r' || ch == '\t') ch = ' ';
            eb.insert(paste);
            edit = true;
            break;
        }
        case Key::Z: if (!ctrl) return false; edit = sh ? eb.redo() : eb.undo(); break;
        case Key::Y: if (!ctrl) return false; edit = eb.redo(); break;
        case Key::Enter: ui.emit(ec.node, SignalKind::Submitted, {.text = eb.text()}); return true;
        default: return false;
        }
        if (edit) changed(ui, ec.node);
        else ui.setValue(ec.node, ui.value(ec.node) + 1.f); // caret moved: repaint (value counts caret moves)
        return true;
    }
};

// ---- Image / Separator ---------------------------------------------------------------------------------------------
struct ImageW {
    static constexpr std::string_view kName = "image";
    static constexpr A11yRole kRole = A11yRole::Image;
    static void paint(const PaintContext& pc, DrawList& dl) {
        DrawCmd c;
        c.op = DrawOp::Image;
        c.image = u32(pc.ui.userData(pc.node));
        c.rect = pc.content;
        c.fill = Color{1.f, 1.f, 1.f, pc.opacity};
        c.node = pc.node.index;
        dl.push(c);
    }
};
struct Separator {
    static constexpr std::string_view kName = "separator";
    static constexpr A11yRole kRole = A11yRole::Separator;
    static Size measure(UiContext&, NodeHandle, Size) { return {1.f, 1.f}; }
    static void paint(const PaintContext& pc, DrawList& dl) {
        const Rect c = pc.content;
        const Color col = pc.tint(pc.style.borderColor.a > 0.f ? pc.style.borderColor : Color::rgb(0x44444F));
        if (c.w >= c.h) dl.rect({c.x, std::round(c.y + c.h * 0.5f - 0.5f), c.w, 1.f}, col);
        else dl.rect({std::round(c.x + c.w * 0.5f - 0.5f), c.y, 1.f, c.h}, col);
    }
};

NodeHandle make(UiContext& ui, std::string_view cls, NodeHandle parent, std::string_view id) {
    auto h = ui.create(cls, parent, id);
    return h ? *h : NodeHandle{};
}

} // namespace

void registerTierA(UiContext& ui) {
    WidgetRegistry& r = ui.registry();
    r.registerWidget<Button>();
    r.registerWidget<ToolButton>();
    r.registerWidget<Link>();
    r.registerWidget<CheckBox>();
    r.registerWidget<RadioButton>();
    r.registerWidget<Switch>();
    r.registerWidget<Slider>();
    r.registerWidget<ScrollBar>();
    r.registerWidget<Dial>();
    r.registerWidget<ProgressBar>();
    r.registerWidget<SpinBox>();
    r.registerWidget<ComboBox>();
    r.registerWidget<LineEdit>();
    r.registerWidget<ImageW>();
    r.registerWidget<Separator>();
}

void install(UiContext& ui) {
    registerTierA(ui);
    registerTierB(ui);
    registerTierCD(ui);
    registerTierD2(ui);
    (void)ui.addStyleSheet(defaultThemeCss());
}

u32 installClasses(UiContext& ui, std::span<const std::string_view> classes, bool theme) {
    // every library class, registered once into a scratch context and copied from there by name
    static const WidgetRegistry& all = [] () -> const WidgetRegistry& {
        static UiContext scratch({.viewport = {1.f, 1.f}, .font = nullptr, .reserveNodes = 4, .atlasSize = 64});
        registerTierA(scratch);
        registerTierB(scratch);
        registerTierCD(scratch);
        registerTierD2(scratch);
        return scratch.registry();
    }();
    u32 unknown = 0;
    for (std::string_view name : classes) {
        if (ui.registry().find(name) != 0xFFFFFFFFu) continue;
        const u32 id = all.find(name);
        if (id == 0xFFFFFFFFu) { ++unknown; continue; }
        ui.registry().add(all.get(id));
    }
    if (theme) (void)ui.addStyleSheet(defaultThemeCss());
    return unknown;
}

NodeHandle panel(UiContext& ui, NodeHandle p, std::string_view id) { return make(ui, "panel", p, id); }
NodeHandle label(UiContext& ui, NodeHandle p, std::string_view t, std::string_view id) {
    const NodeHandle n = make(ui, "label", p, id);
    ui.setText(n, t);
    return n;
}
NodeHandle button(UiContext& ui, NodeHandle p, std::string_view t, std::string_view id) {
    const NodeHandle n = make(ui, "button", p, id);
    ui.setText(n, t);
    return n;
}
NodeHandle toolButton(UiContext& ui, NodeHandle p, std::string_view t, std::string_view id) {
    const NodeHandle n = make(ui, "toolbutton", p, id);
    ui.setText(n, t);
    return n;
}
NodeHandle link(UiContext& ui, NodeHandle p, std::string_view t, std::string_view id) {
    const NodeHandle n = make(ui, "link", p, id);
    ui.setText(n, t);
    return n;
}
NodeHandle checkBox(UiContext& ui, NodeHandle p, std::string_view t, bool on, std::string_view id) {
    const NodeHandle n = make(ui, "checkbox", p, id);
    ui.setText(n, t);
    ui.setChecked(n, on);
    return n;
}
NodeHandle radioButton(UiContext& ui, NodeHandle p, std::string_view t, bool on, std::string_view id) {
    const NodeHandle n = make(ui, "radio", p, id);
    ui.setText(n, t);
    ui.setChecked(n, on);
    return n;
}
NodeHandle toggleSwitch(UiContext& ui, NodeHandle p, std::string_view t, bool on, std::string_view id) {
    const NodeHandle n = make(ui, "switch", p, id);
    ui.setText(n, t);
    ui.setChecked(n, on);
    return n;
}
NodeHandle slider(UiContext& ui, NodeHandle p, f32 lo, f32 hi, f32 v, f32 step, std::string_view id) {
    const NodeHandle n = make(ui, "slider", p, id);
    ui.setRange(n, lo, hi, step);
    ui.setValue(n, v);
    return n;
}
NodeHandle scrollBar(UiContext& ui, NodeHandle p, bool vertical, std::string_view id) {
    const NodeHandle n = make(ui, "scrollbar", p, id);
    ui.setRange(n, 0.f, 1.f, 0.f);
    if (vertical) ui.setCss(n, "width", "12px");
    else ui.setCss(n, "height", "12px");
    return n;
}
NodeHandle spinBox(UiContext& ui, NodeHandle p, f32 lo, f32 hi, f32 v, f32 step, std::string_view id) {
    const NodeHandle n = make(ui, "spinbox", p, id);
    ui.setRange(n, lo, hi, step);
    ui.setValue(n, v);
    return n;
}
NodeHandle comboBox(UiContext& ui, NodeHandle p, std::string_view items, u32 sel, std::string_view id) {
    const NodeHandle n = make(ui, "combobox", p, id);
    ui.setText(n, items);
    ui.setRange(n, 0.f, f32(std::max(1u, itemCount(items)) - 1), 1.f);
    ui.setValue(n, f32(sel));
    return n;
}
NodeHandle lineEdit(UiContext& ui, NodeHandle p, std::string_view t, std::string_view id) {
    const NodeHandle n = make(ui, "lineedit", p, id);
    ui.setText(n, t);
    ui.setRange(n, 0.f, 1e9f, 0.f);
    ui.editBuffer(n).setText(t);
    ui.editBuffer(n).end(false);
    return n;
}
NodeHandle progressBar(UiContext& ui, NodeHandle p, f32 v, std::string_view id) {
    const NodeHandle n = make(ui, "progressbar", p, id);
    ui.setRange(n, 0.f, 1.f);
    ui.setValue(n, v);
    return n;
}
NodeHandle dial(UiContext& ui, NodeHandle p, f32 lo, f32 hi, f32 v, std::string_view id) {
    const NodeHandle n = make(ui, "dial", p, id);
    ui.setRange(n, lo, hi);
    ui.setValue(n, v);
    return n;
}
NodeHandle image(UiContext& ui, NodeHandle p, u32 idx, f32 w, f32 h, std::string_view id) {
    const NodeHandle n = make(ui, "image", p, id);
    ui.setUserData(n, idx);
    ui.set(n, Prop::Width, PropValue::ofLength(Length::px(w)));
    ui.set(n, Prop::Height, PropValue::ofLength(Length::px(h)));
    return n;
}
NodeHandle separator(UiContext& ui, NodeHandle p, std::string_view id) { return make(ui, "separator", p, id); }

std::string_view comboText(const UiContext& ui, NodeHandle n) { return item(ui.text(n), u32(ui.value(n))); }

} // namespace fuse::ui::widgets
