// FUSE UI Tier C (model / view: sort-filter proxy, grid view; delegates live with the table in widgets_tier_b2.cpp)
// and Tier D (property grid, colour picker, calendar, chart, markdown view). Plan §5.
#include "fuse/ui/widgets/widgets.hpp"
#include "widgets_internal.hpp"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstdio>
#include <memory>
#include <numeric>
#include <string>
#include <vector>

namespace fuse::ui::widgets {
namespace {

NodeHandle make(UiContext& ui, std::string_view cls, NodeHandle parent, std::string_view id = {}) {
    auto h = ui.create(cls, parent, id);
    return h ? *h : NodeHandle{};
}
f32 lineH(const UiContext& ui, NodeHandle n) {
    const f32 px = ui.computed(n).fontSize;
    return std::ceil(ui.font() ? ui.font()->metrics(px).lineHeight() : px * 1.25f);
}
Color accentOf(const PaintContext& pc) { return pc.tint(pc.style.background2.a > 0.f ? pc.style.background2 : Color::rgb(0x3D8BFD)); }
NodeHandle nthChild(const UiContext& ui, NodeHandle n, u32 i) {
    NodeHandle c = ui.firstChild(n);
    while (i-- && ui.alive(c)) c = ui.nextSibling(c);
    return c;
}
bool parseNum(std::string_view s, double& out) {
    const auto [p, ec] = std::from_chars(s.data(), s.data() + s.size(), out);
    return ec == std::errc{} && p == s.data() + s.size() && !s.empty();
}

/// Per-node state pool keyed by the node's user data (slot + 1); a slot is reused once its node is gone. One pool per
/// state type; the UI is single-threaded.
template <class S>
struct Pool {
    struct Slot { NodeHandle node; std::unique_ptr<S> s; };
    std::vector<Slot> slots;
    S* get(const UiContext& ui, NodeHandle n) {
        const u64 k = ui.alive(n) ? ui.userData(n) : 0;
        if (k == 0 || k > slots.size() || slots[k - 1].node != n) return nullptr;
        return slots[k - 1].s.get();
    }
    S& create(UiContext& ui, NodeHandle n) {
        for (std::size_t i = 0; i < slots.size(); ++i)
            if (!ui.alive(slots[i].node) || ui.userData(slots[i].node) != i + 1) {
                slots[i] = {n, std::make_unique<S>()};
                ui.setUserData(n, i + 1);
                return *slots[i].s;
            }
        slots.push_back({n, std::make_unique<S>()});
        ui.setUserData(n, slots.size());
        return *slots.back().s;
    }
};

// ---- grid view ------------------------------------------------------------------------------------------------------
struct GridState {
    const ItemModel* model = nullptr;
    f32 tileW = 96.f, tileH = 72.f;
    u32 first = 0; ///< first visible tile row
    std::string scratch;
};
Pool<GridState>& gridPool() { static Pool<GridState> p; return p; }

struct GridView {
    static constexpr std::string_view kName = "gridview";
    static constexpr A11yRole kRole = A11yRole::List;
    static constexpr bool kFocusable = true;
    static constexpr bool kInteractive = true;
    static constexpr f32 kBar = 8.f;
    static u32 count(const GridState& s) { return s.model && s.model->rows ? s.model->rows(s.model->user) : 0u; }
    static Size measure(UiContext&, NodeHandle, Size) { return {320.f, 220.f}; }
    static u32 cols(const UiContext& ui, NodeHandle n, const GridState& s) {
        return std::max(1u, u32((ui.contentRect(n).w - kBar) / s.tileW));
    }
    static u32 rowsTotal(const UiContext& ui, NodeHandle n, const GridState& s) {
        const u32 c = cols(ui, n, s);
        return (count(s) + c - 1) / c;
    }
    static u32 visRows(const UiContext& ui, NodeHandle n, const GridState& s) {
        return std::max(1u, u32(ui.contentRect(n).h / s.tileH));
    }
    static void setFirst(UiContext& ui, NodeHandle n, GridState& s, i64 f) {
        const u32 r = rowsTotal(ui, n, s), v = visRows(ui, n, s);
        const u32 nf = u32(std::clamp<i64>(f, 0, r > v ? i64(r - v) : 0));
        if (nf != s.first) { s.first = nf; ui.invalidate(n); }
    }
    static void select(UiContext& ui, NodeHandle n, GridState& s, i64 idx) {
        const u32 total = count(s);
        if (!total) return;
        idx = std::clamp<i64>(idx, 0, i64(total) - 1);
        const u32 c = cols(ui, n, s), row = u32(idx) / c, v = visRows(ui, n, s);
        if (row < s.first) setFirst(ui, n, s, row);
        else if (row >= s.first + v) setFirst(ui, n, s, i64(row) - v + 1);
        ui.setValue(n, f32(idx), true);
    }
    static void paint(const PaintContext& pc, DrawList& dl) {
        UiContext& ui = pc.ui;
        GridState* s = gridPool().get(ui, pc.node);
        if (!s) return;
        const Rect c = pc.content;
        const u32 nc = cols(ui, pc.node, *s), total = count(*s), vis = visRows(ui, pc.node, *s);
        const Color acc = accentOf(pc), text = pc.tint(pc.style.color);
        const i32 sel = i32(ui.value(pc.node));
        dl.pushClip(c);
        for (u32 r = 0; r <= vis; ++r)
            for (u32 k = 0; k < nc; ++k) {
                const u32 idx = (s->first + r) * nc + k;
                if (idx >= total) break;
                const Rect t{c.x + f32(k) * s->tileW, c.y + f32(r) * s->tileH, s->tileW, s->tileH};
                if (t.y >= c.bottom()) break;
                if (i32(idx) == sel) dl.rect({t.x + 2.f, t.y + 2.f, t.w - 4.f, t.h - 4.f}, acc.withAlpha(acc.a * 0.45f), 5.f);
                // icon: a rounded tile tinted by index
                const f32 hue = f32((idx * 47u) % 360u) / 360.f;
                const Color ic{0.35f + 0.3f * std::abs(std::sin(hue * 6.283f)), 0.35f + 0.3f * std::abs(std::sin(hue * 6.283f + 2.f)),
                               0.45f + 0.3f * std::abs(std::sin(hue * 6.283f + 4.f)), 1.f};
                const f32 iw = std::min(t.w - 24.f, t.h - 30.f);
                dl.rect({t.x + (t.w - iw) * 0.5f, t.y + 6.f, iw, iw}, pc.tint(ic), 6.f);
                const std::string_view label = s->model->data(s->model->user, idx, 0, s->scratch);
                const Rect lr{t.x + 4.f, t.y + 8.f + iw, t.w - 8.f, t.h - iw - 10.f};
                dl.pushClip(lr);
                ui.paintString(dl, label, lr, text, pc.style.fontSize * 0.86f, TextAlignment::Center, pc.node.index);
                dl.popClip();
            }
        const u32 rt = rowsTotal(ui, pc.node, *s);
        if (rt > vis) {
            const Rect tr{c.right() - kBar, c.y, kBar, c.h};
            const f32 tl = std::max(16.f, tr.h * f32(vis) / f32(rt));
            dl.rect(tr, pc.tint(Color::rgb(0x000000, 0.15f)), kBar * 0.5f);
            dl.rect({tr.x, tr.y + (tr.h - tl) * f32(s->first) / f32(rt - vis), kBar, tl}, pc.tint(Color::rgb(0x8A8A96, 0.85f)), kBar * 0.5f);
        }
        dl.popClip();
    }
    static bool onEvent(EventContext& ec, const Event& e) {
        if (ec.phase != Phase::Target) return false;
        UiContext& ui = ec.ui;
        GridState* s = gridPool().get(ui, ec.node);
        if (!s) return false;
        const Rect c = ui.contentRect(ec.node);
        const i64 nc = cols(ui, ec.node, *s);
        switch (e.type) {
        case EventType::PointerDown: {
            if (e.pos.x >= c.right() - kBar) return true;
            const i64 k = i64((e.pos.x - c.x) / s->tileW), r = i64((e.pos.y - c.y) / s->tileH) + s->first;
            if (k < nc && k >= 0 && r >= 0) {
                const i64 idx = r * nc + k;
                if (idx < i64(count(*s))) select(ui, ec.node, *s, idx);
            }
            return true;
        }
        case EventType::Wheel: setFirst(ui, ec.node, *s, i64(s->first) - i64(std::lround(e.wheel.y))); return true;
        case EventType::KeyDown: {
            const i64 cur = i64(ui.value(ec.node));
            switch (e.key) {
            case Key::Right: select(ui, ec.node, *s, cur + 1); return true;
            case Key::Left: select(ui, ec.node, *s, cur < 0 ? 0 : cur - 1); return true;
            case Key::Down: select(ui, ec.node, *s, cur < 0 ? 0 : cur + nc); return true;
            case Key::Up: select(ui, ec.node, *s, cur - nc); return true;
            case Key::PageDown: select(ui, ec.node, *s, cur + nc * visRows(ui, ec.node, *s)); return true;
            case Key::PageUp: select(ui, ec.node, *s, cur - nc * visRows(ui, ec.node, *s)); return true;
            case Key::Home: select(ui, ec.node, *s, 0); return true;
            case Key::End: select(ui, ec.node, *s, i64(count(*s)) - 1); return true;
            case Key::Enter: if (cur >= 0) ui.emit(ec.node, SignalKind::Submitted, {.value = f32(cur)}); return true;
            default: return false;
            }
        }
        default: return false;
        }
    }
};

// ---- colour picker ----------------------------------------------------------------------------------------------------
// HSVA in the node's user data, 16 bits each (h in [0, 1)).
struct Hsva { f32 h, s, v, a; };
Hsva unpack(u64 u) {
    auto f = [&](int sh) { return f32((u >> sh) & 0xFFFFu) / 65535.f; };
    return {f(48), f(32), f(16), f(0)};
}
u64 pack(Hsva c) {
    auto q = [](f32 x) { return u64(std::lround(std::clamp(x, 0.f, 1.f) * 65535.f)); };
    return (q(c.h) << 48) | (q(c.s) << 32) | (q(c.v) << 16) | q(c.a);
}
Color hsvToRgb(f32 h, f32 s, f32 v, f32 a = 1.f) {
    h = (h - std::floor(h)) * 6.f;
    const int i = int(h) % 6;
    const f32 f = h - std::floor(h), p = v * (1.f - s), q = v * (1.f - s * f), t = v * (1.f - s * (1.f - f));
    switch (i) {
    case 0: return {v, t, p, a};
    case 1: return {q, v, p, a};
    case 2: return {p, v, t, a};
    case 3: return {p, q, v, a};
    case 4: return {t, p, v, a};
    default: return {v, p, q, a};
    }
}
Hsva rgbToHsv(Color c) {
    const f32 mx = std::max({c.r, c.g, c.b}), mn = std::min({c.r, c.g, c.b}), d = mx - mn;
    f32 h = 0.f;
    if (d > 0.f) {
        if (mx == c.r) h = std::fmod((c.g - c.b) / d, 6.f);
        else if (mx == c.g) h = (c.b - c.r) / d + 2.f;
        else h = (c.r - c.g) / d + 4.f;
        h /= 6.f;
        if (h < 0.f) h += 1.f;
    }
    return {h, mx > 0.f ? d / mx : 0.f, mx, c.a};
}
u8 pickZone = 0; // 1 square, 2 hue, 3 alpha (single pointer, single thread)

struct ColorPickerW {
    static constexpr std::string_view kName = "colorpicker";
    static constexpr A11yRole kRole = A11yRole::Group;
    static constexpr bool kFocusable = true;
    static constexpr bool kInteractive = true;
    static Size measure(UiContext&, NodeHandle, Size) { return {232.f, 196.f}; }
    struct Geo { Rect sq, hue, alpha, swatch; };
    static Geo geo(Rect c) {
        Geo g;
        g.sq = {c.x, c.y, c.w - 26.f, c.h - 22.f};
        g.hue = {c.right() - 16.f, c.y, 16.f, g.sq.h};
        g.alpha = {c.x, c.bottom() - 14.f, g.sq.w, 14.f};
        g.swatch = {c.right() - 16.f, c.bottom() - 14.f, 16.f, 14.f};
        return g;
    }
    static void publish(UiContext& ui, NodeHandle n, Hsva h) {
        ui.setUserData(n, pack(h));
        const Color c = hsvToRgb(h.h, h.s, h.v, h.a);
        char b[16];
        auto q = [](f32 x) { return unsigned(std::lround(std::clamp(x, 0.f, 1.f) * 255.f)); };
        std::snprintf(b, sizeof b, "#%02X%02X%02X%02X", q(c.r), q(c.g), q(c.b), q(c.a));
        ui.setText(n, b);
        ui.setValue(n, h.h * 360.f, true);
        ui.emit(n, SignalKind::TextChanged, {.text = ui.text(n)});
        ui.invalidate(n);
    }
    static void paint(const PaintContext& pc, DrawList& dl) {
        const Hsva h = unpack(pc.ui.userData(pc.node));
        const Geo g = geo(pc.content);
        const Color hueCol = hsvToRgb(h.h, 1.f, 1.f);
        dl.gradientRect(g.sq, pc.tint(Color::rgb(0xFFFFFF)), pc.tint(hueCol), false, {3.f, 3.f, 3.f, 3.f});
        dl.gradientRect(g.sq, pc.tint(Color::rgb(0x000000, 0.f)), pc.tint(Color::rgb(0x000000)), true, {3.f, 3.f, 3.f, 3.f});
        for (int i = 0; i < 6; ++i) {
            const Rect seg{g.hue.x, g.hue.y + g.hue.h * f32(i) / 6.f, g.hue.w, g.hue.h / 6.f + 0.5f};
            dl.gradientRect(seg, pc.tint(hsvToRgb(f32(i) / 6.f, 1.f, 1.f)), pc.tint(hsvToRgb(f32(i + 1) / 6.f, 1.f, 1.f)), true);
        }
        const Color rgb = hsvToRgb(h.h, h.s, h.v);
        for (int i = 0; i < int(g.alpha.w / 7.f); ++i) // checker under the alpha ramp
            dl.rect({g.alpha.x + f32(i) * 7.f, g.alpha.y + ((i & 1) ? 0.f : 7.f), 7.f, 7.f}, pc.tint(Color::rgb(0x55555F)));
        dl.gradientRect(g.alpha, pc.tint(rgb.withAlpha(0.f)), pc.tint(rgb), false, {3.f, 3.f, 3.f, 3.f});
        dl.rect(g.swatch, pc.tint(hsvToRgb(h.h, h.s, h.v, h.a)), 3.f);
        auto ring = [&](Vec2 p) {
            dl.roundedRect({p.x - 6.f, p.y - 6.f, 12.f, 12.f}, {}, {6.f, 6.f, 6.f, 6.f}, pc.tint(Color::rgb(0x000000, 0.8f)), 3.f);
            dl.roundedRect({p.x - 5.f, p.y - 5.f, 10.f, 10.f}, {}, {5.f, 5.f, 5.f, 5.f}, pc.tint(Color::rgb(0xFFFFFF)), 1.5f);
        };
        ring({g.sq.x + h.s * g.sq.w, g.sq.y + (1.f - h.v) * g.sq.h});
        dl.roundedRect({g.hue.x - 2.f, g.hue.y + h.h * g.hue.h - 3.f, g.hue.w + 4.f, 6.f}, {}, {2.f, 2.f, 2.f, 2.f}, pc.tint(Color::rgb(0xFFFFFF)), 1.5f);
        dl.roundedRect({g.alpha.x + h.a * g.alpha.w - 3.f, g.alpha.y - 2.f, 6.f, g.alpha.h + 4.f}, {}, {2.f, 2.f, 2.f, 2.f}, pc.tint(Color::rgb(0xFFFFFF)), 1.5f);
        if (pc.states & kStateFocus) dl.roundedRect(pc.rect, {}, {5.f, 5.f, 5.f, 5.f}, accentOf(pc), 1.f);
    }
    static void apply(UiContext& ui, NodeHandle n, Vec2 p) {
        const Geo g = geo(ui.contentRect(n));
        Hsva h = unpack(ui.userData(n));
        auto t = [](f32 a, f32 lo, f32 len) { return std::clamp((a - lo) / std::max(1.f, len), 0.f, 1.f); };
        if (pickZone == 1) { h.s = t(p.x, g.sq.x, g.sq.w); h.v = 1.f - t(p.y, g.sq.y, g.sq.h); }
        else if (pickZone == 2) h.h = std::min(t(p.y, g.hue.y, g.hue.h), 0.9999f);
        else if (pickZone == 3) h.a = t(p.x, g.alpha.x, g.alpha.w);
        else return;
        publish(ui, n, h);
    }
    static bool onEvent(EventContext& ec, const Event& e) {
        if (ec.phase != Phase::Target) return false;
        UiContext& ui = ec.ui;
        if (e.type == EventType::PointerDown) {
            const Geo g = geo(ui.contentRect(ec.node));
            pickZone = g.sq.contains(e.pos) ? 1 : g.hue.contains(e.pos) ? 2 : g.alpha.contains(e.pos) ? 3 : 0;
            apply(ui, ec.node, e.pos);
            return true;
        }
        if (e.type == EventType::PointerMove && ui.pressed() == ec.node) { apply(ui, ec.node, e.pos); return true; }
        if (e.type == EventType::PointerUp) { pickZone = 0; return false; }
        if (e.type == EventType::KeyDown) {
            Hsva h = unpack(ui.userData(ec.node));
            switch (e.key) {
            case Key::Left: h.s = std::max(0.f, h.s - 0.05f); break;
            case Key::Right: h.s = std::min(1.f, h.s + 0.05f); break;
            case Key::Up: h.v = std::min(1.f, h.v + 0.05f); break;
            case Key::Down: h.v = std::max(0.f, h.v - 0.05f); break;
            case Key::PageUp: h.h = std::fmod(h.h + 1.f - 1.f / 36.f, 1.f); break;
            case Key::PageDown: h.h = std::fmod(h.h + 1.f / 36.f, 1.f); break;
            default: return false;
            }
            publish(ui, ec.node, h);
            return true;
        }
        return false;
    }
};

// ---- calendar ---------------------------------------------------------------------------------------------------------
constexpr const char* kMonths[] = {"January", "February", "March", "April", "May", "June", "July", "August", "September",
                                   "October", "November", "December"};
constexpr const char* kDays[] = {"Mo", "Tu", "We", "Th", "Fr", "Sa", "Su"};
i32 weekday(i32 z) { return ((z + 3) % 7 + 7) % 7; } // Monday = 0 (1970-01-01 was a Thursday)
int daysIn(int y, int m) {
    static constexpr int d[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    const bool leap = (y % 4 == 0 && y % 100 != 0) || y % 400 == 0;
    return m == 2 && leap ? 29 : d[m - 1];
}

struct CalendarW {
    static constexpr std::string_view kName = "calendar";
    static constexpr A11yRole kRole = A11yRole::Group;
    static constexpr bool kFocusable = true;
    static constexpr bool kInteractive = true;
    static Size measure(UiContext& ui, NodeHandle n, Size) { return {7.f * 34.f, lineH(ui, n) + 8.f + 7.f * 26.f}; }
    static i32 shownMonth(const UiContext& ui, NodeHandle n) { return i32(ui.userData(n)) - 1000000; } // y * 12 + (m - 1)
    static void setShown(UiContext& ui, NodeHandle n, i32 ym) { ui.setUserData(n, u64(ym + 1000000)); ui.invalidate(n); }
    struct Geo { Rect head, prev, next; f32 cellW, cellH, gridY; };
    static Geo geo(const UiContext& ui, NodeHandle n) {
        const Rect c = ui.contentRect(n);
        Geo g;
        const f32 hh = lineH(ui, n) + 8.f;
        g.head = {c.x, c.y, c.w, hh};
        g.prev = {c.x, c.y, hh, hh};
        g.next = {c.right() - hh, c.y, hh, hh};
        g.cellW = c.w / 7.f;
        g.cellH = (c.h - hh) / 7.f;
        g.gridY = c.y + hh + g.cellH; // first week row (below the weekday names)
        return g;
    }
    static i32 firstCellDay(i32 ym) { // day number of the grid's top-left cell
        const i32 first = daysFromCivil(ym / 12, ym % 12 + 1, 1);
        return first - weekday(first);
    }
    static void select(UiContext& ui, NodeHandle n, i32 z) {
        int y, m, d;
        civilFromDays(z, y, m, d);
        setShown(ui, n, y * 12 + m - 1);
        ui.setValue(n, f32(z), true);
    }
    static void paint(const PaintContext& pc, DrawList& dl) {
        UiContext& ui = pc.ui;
        const Geo g = geo(ui, pc.node);
        const i32 ym = shownMonth(ui, pc.node), sel = i32(ui.value(pc.node));
        const Color text = pc.tint(pc.style.color), dim = pc.tint(pc.style.color.withAlpha(pc.style.color.a * 0.38f)), acc = accentOf(pc);
        const f32 fs = pc.style.fontSize;
        char b[48];
        std::snprintf(b, sizeof b, "%s %d", kMonths[ym % 12], ym / 12);
        ui.paintString(dl, b, g.head, text, fs, TextAlignment::Center, pc.node.index);
        for (int side = 0; side < 2; ++side) { // chevrons
            const Rect r = side ? g.next : g.prev;
            const f32 cx = r.x + r.w * 0.5f, cy = r.y + r.h * 0.5f, d = side ? 3.f : -3.f;
            dl.line({cx - d, cy - 5.f}, {cx + d, cy}, text, 1.5f);
            dl.line({cx + d, cy}, {cx - d, cy + 5.f}, text, 1.5f);
        }
        for (int k = 0; k < 7; ++k)
            ui.paintString(dl, kDays[k], {g.head.x + f32(k) * g.cellW, g.head.bottom(), g.cellW, g.cellH}, k >= 5 ? dim : text,
                           fs * 0.85f, TextAlignment::Center, pc.node.index);
        const i32 z0 = firstCellDay(ym);
        for (int i = 0; i < 42; ++i) {
            const i32 z = z0 + i;
            int y, m, d;
            civilFromDays(z, y, m, d);
            const Rect cell{g.head.x + f32(i % 7) * g.cellW, g.gridY + f32(i / 7) * g.cellH, g.cellW, g.cellH};
            const bool inMonth = y * 12 + m - 1 == ym;
            if (z == sel) dl.rect({cell.x + 2.f, cell.y + 1.f, cell.w - 4.f, cell.h - 2.f}, acc, 4.f);
            std::snprintf(b, sizeof b, "%d", d);
            ui.paintString(dl, b, cell, z == sel ? pc.tint(Color::rgb(0xFFFFFF)) : inMonth ? text : dim, fs * 0.9f, TextAlignment::Center,
                           pc.node.index);
        }
        if (pc.states & kStateFocus) dl.roundedRect(pc.rect, {}, {5.f, 5.f, 5.f, 5.f}, acc.withAlpha(acc.a * 0.6f), 1.f);
    }
    static void pageMonth(UiContext& ui, NodeHandle n, i32 delta, bool moveSelection) {
        const i32 ym = shownMonth(ui, n) + delta;
        if (!moveSelection) { setShown(ui, n, ym); return; }
        int y, m, d;
        civilFromDays(i32(ui.value(n)), y, m, d);
        const int ny = ym / 12, nm = ym % 12 + 1;
        select(ui, n, daysFromCivil(ny, nm, std::min(d, daysIn(ny, nm))));
    }
    static bool onEvent(EventContext& ec, const Event& e) {
        if (ec.phase != Phase::Target) return false;
        UiContext& ui = ec.ui;
        if (e.type == EventType::PointerDown) {
            const Geo g = geo(ui, ec.node);
            if (g.prev.contains(e.pos)) { pageMonth(ui, ec.node, -1, false); return true; }
            if (g.next.contains(e.pos)) { pageMonth(ui, ec.node, 1, false); return true; }
            if (e.pos.y >= g.gridY) {
                const i32 row = i32((e.pos.y - g.gridY) / g.cellH), col = i32((e.pos.x - g.head.x) / g.cellW);
                if (row >= 0 && row < 6 && col >= 0 && col < 7) select(ui, ec.node, firstCellDay(shownMonth(ui, ec.node)) + row * 7 + col);
            }
            return true;
        }
        if (e.type == EventType::KeyDown) {
            const i32 cur = i32(ui.value(ec.node));
            switch (e.key) {
            case Key::Left: select(ui, ec.node, cur - 1); return true;
            case Key::Right: select(ui, ec.node, cur + 1); return true;
            case Key::Up: select(ui, ec.node, cur - 7); return true;
            case Key::Down: select(ui, ec.node, cur + 7); return true;
            case Key::PageUp: pageMonth(ui, ec.node, -1, true); return true;
            case Key::PageDown: pageMonth(ui, ec.node, 1, true); return true;
            case Key::Enter: ui.emit(ec.node, SignalKind::Submitted, {.value = f32(cur)}); return true;
            default: return false;
            }
        }
        return false;
    }
};

// ---- chart ----------------------------------------------------------------------------------------------------------------
struct Series { std::string name; std::vector<f32> v; Color color; };
struct ChartState { ChartKind kind = ChartKind::Line; std::vector<Series> series; };
Pool<ChartState>& chartPool() { static Pool<ChartState> p; return p; }

struct ChartW {
    static constexpr std::string_view kName = "chart";
    static constexpr A11yRole kRole = A11yRole::Image;
    static constexpr bool kFocusable = true;
    static constexpr bool kInteractive = true;
    static Size measure(UiContext&, NodeHandle, Size) { return {360.f, 200.f}; }
    static u32 points(const ChartState& s) {
        u32 n = 0;
        for (const Series& se : s.series) n = std::max(n, u32(se.v.size()));
        return n;
    }
    static Rect plot(const UiContext& ui, NodeHandle n) {
        const Rect c = ui.contentRect(n);
        const f32 lh = lineH(ui, n);
        return {c.x + 40.f, c.y + lh + 6.f, c.w - 46.f, c.h - lh - 12.f};
    }
    static void range(const ChartState& s, f32& lo, f32& hi) {
        lo = 1e30f; hi = -1e30f;
        for (const Series& se : s.series) for (f32 v : se.v) { lo = std::min(lo, v); hi = std::max(hi, v); }
        if (lo > hi) { lo = 0.f; hi = 1.f; }
        if (s.kind == ChartKind::Bar || s.kind == ChartKind::Area) { lo = std::min(lo, 0.f); hi = std::max(hi, 0.f); }
        if (hi - lo < 1e-6f) hi = lo + 1.f;
        // nice step: 1, 2 or 5 x 10^k for ~4 intervals
        const f32 raw = (hi - lo) / 4.f, mag = std::pow(10.f, std::floor(std::log10(raw)));
        const f32 st = raw / mag <= 1.f ? mag : raw / mag <= 2.f ? 2.f * mag : raw / mag <= 5.f ? 5.f * mag : 10.f * mag;
        lo = std::floor(lo / st) * st;
        hi = std::ceil(hi / st) * st;
    }
    static f32 xAt(const Rect& p, u32 i, u32 n, bool bars) {
        return bars ? p.x + (f32(i) + 0.5f) * p.w / f32(std::max(1u, n)) : p.x + (n > 1 ? f32(i) * p.w / f32(n - 1) : p.w * 0.5f);
    }
    static void paint(const PaintContext& pc, DrawList& dl) {
        UiContext& ui = pc.ui;
        ChartState* s = chartPool().get(ui, pc.node);
        if (!s) return;
        const Rect p = plot(ui, pc.node), c = pc.content;
        const Color text = pc.tint(pc.style.color), dim = pc.tint(pc.style.color.withAlpha(pc.style.color.a * 0.55f));
        const f32 fs = pc.style.fontSize * 0.8f;
        f32 lo, hi;
        range(*s, lo, hi);
        auto yOf = [&](f32 v) { return p.bottom() - (v - lo) / (hi - lo) * p.h; };
        char b[32];
        for (int i = 0; i <= 4; ++i) {
            const f32 v = lo + (hi - lo) * f32(i) / 4.f, y = std::round(yOf(v));
            dl.rect({p.x, y, p.w, 1.f}, pc.tint(Color::rgb(0xFFFFFF, i == 0 ? 0.25f : 0.07f)));
            std::snprintf(b, sizeof b, "%.4g", double(v));
            ui.paintString(dl, b, {c.x, y - 8.f, 36.f, 16.f}, dim, fs, TextAlignment::End, pc.node.index);
        }
        const u32 n = points(*s);
        const bool bars = s->kind == ChartKind::Bar;
        const u32 ns = u32(s->series.size());
        dl.pushClip({p.x, p.y - 4.f, p.w, p.h + 5.f});
        for (u32 k = 0; k < ns; ++k) {
            const Series& se = s->series[k];
            const Color col = pc.tint(se.color);
            for (u32 i = 0; i < se.v.size(); ++i) {
                const f32 x = xAt(p, i, n, bars), y = yOf(se.v[i]);
                switch (s->kind) {
                case ChartKind::Bar: {
                    const f32 slot = p.w / f32(std::max(1u, n)), bw = slot * 0.8f / f32(std::max(1u, ns));
                    const f32 bx = x - slot * 0.4f + f32(k) * bw, y0 = yOf(0.f);
                    dl.rect({bx + 1.f, std::min(y, y0), bw - 2.f, std::max(1.f, std::abs(y0 - y))}, col, 2.f);
                    break;
                }
                case ChartKind::Scatter:
                    dl.rect({x - 3.f, y - 3.f, 6.f, 6.f}, col, 3.f);
                    break;
                case ChartKind::Area:
                    if (i + 1 < se.v.size()) { // filled with 2 px columns under the segment
                        const f32 x1 = xAt(p, i + 1, n, false), y1 = yOf(se.v[i + 1]), y0 = yOf(0.f);
                        for (f32 cx = x; cx < x1; cx += 2.f) {
                            const f32 t = (cx - x) / std::max(1.f, x1 - x), cy = y + (y1 - y) * t;
                            dl.rect({cx, std::min(cy, y0), std::min(2.f, x1 - cx), std::abs(y0 - cy)}, col.withAlpha(col.a * 0.35f));
                        }
                    }
                    [[fallthrough]];
                case ChartKind::Line:
                    if (i + 1 < se.v.size()) dl.line({x, y}, {xAt(p, i + 1, n, false), yOf(se.v[i + 1])}, col, 2.f);
                    break;
                }
            }
        }
        dl.popClip();
        // legend (top right) and hover readout (top left)
        f32 lx = c.right();
        for (u32 k = ns; k-- > 0;) {
            const Series& se = s->series[k];
            const f32 w = ui.measureString(se.name, fs) + 22.f;
            lx -= w;
            dl.rect({lx, c.y + 5.f, 10.f, 10.f}, pc.tint(se.color), 2.f);
            ui.paintString(dl, se.name, {lx + 14.f, c.y, w - 14.f, 20.f}, text, fs, TextAlignment::Start, pc.node.index);
        }
        const i32 hov = i32(ui.value(pc.node));
        if (hov >= 0 && u32(hov) < n) {
            const f32 x = std::round(xAt(p, u32(hov), n, bars));
            dl.rect({x, p.y, 1.f, p.h}, pc.tint(Color::rgb(0xFFFFFF, 0.3f)));
            std::string r = "#" + std::to_string(hov);
            for (const Series& se : s->series)
                if (u32(hov) < se.v.size()) { std::snprintf(b, sizeof b, "  %s %.4g", se.name.c_str(), double(se.v[hov])); r += b; }
            ui.paintString(dl, r, {c.x, c.y, c.w, 20.f}, text, fs, TextAlignment::Start, pc.node.index);
        }
    }
    static bool onEvent(EventContext& ec, const Event& e) {
        if (ec.phase != Phase::Target) return false;
        UiContext& ui = ec.ui;
        ChartState* s = chartPool().get(ui, ec.node);
        if (!s) return false;
        if (e.type == EventType::PointerMove) {
            const Rect p = plot(ui, ec.node);
            const u32 n = points(*s);
            i32 idx = -1;
            if (n && p.contains(e.pos)) {
                const bool bars = s->kind == ChartKind::Bar;
                const f32 t = bars ? (e.pos.x - p.x) / p.w * f32(n) - 0.5f : (e.pos.x - p.x) / p.w * f32(n - 1);
                idx = std::clamp(i32(std::lround(t)), 0, i32(n) - 1);
            }
            if (f32(idx) != ui.value(ec.node)) { ui.setValue(ec.node, f32(idx), true); ui.invalidate(ec.node); }
            return false;
        }
        return false;
    }
};

// ---- property grid ------------------------------------------------------------------------------------------------------
void stripInline(std::string& s) {
    std::string o;
    o.reserve(s.size());
    for (std::size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '`') continue;
        if ((s[i] == '*' || s[i] == '_') && i + 1 < s.size() && s[i + 1] == s[i]) { ++i; continue; }
        o += s[i];
    }
    s = std::move(o);
}

} // namespace

void registerTierCD(UiContext& ui) {
    WidgetRegistry& r = ui.registry();
    if (r.find("gridview") != 0xFFFFFFFFu) return;
    r.registerWidget<GridView>();
    r.registerWidget<ColorPickerW>();
    r.registerWidget<CalendarW>();
    r.registerWidget<ChartW>();
}

// ---- civil dates (H. Hinnant's algorithms) ---------------------------------------------------------------------------
i32 daysFromCivil(int y, int m, int d) {
    y -= m <= 2;
    const int era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = unsigned(y - era * 400);
    const unsigned doy = (153u * unsigned(m + (m > 2 ? -3 : 9)) + 2u) / 5u + unsigned(d) - 1u;
    const unsigned doe = yoe * 365u + yoe / 4u - yoe / 100u + doy;
    return era * 146097 + i32(doe) - 719468;
}
void civilFromDays(i32 z, int& y, int& m, int& d) {
    z += 719468;
    const i32 era = (z >= 0 ? z : z - 146096) / 146097;
    const unsigned doe = unsigned(z - era * 146097);
    const unsigned yoe = (doe - doe / 1460u + doe / 36524u - doe / 146096u) / 365u;
    const unsigned doy = doe - (365u * yoe + yoe / 4u - yoe / 100u);
    const unsigned mp = (5u * doy + 2u) / 153u;
    d = int(doy - (153u * mp + 2u) / 5u + 1u);
    m = int(mp < 10 ? mp + 3 : mp - 9);
    y = int(yoe) + era * 400 + (m <= 2);
}

// ---- sort / filter proxy -------------------------------------------------------------------------------------------------
SortFilterProxy::SortFilterProxy(const ItemModel* source) : m_src(source) {
    m_model.user = this;
    m_model.columns = source ? source->columns : 1;
    m_model.rows = [](const void* u) { return static_cast<const SortFilterProxy*>(u)->rows(); };
    m_model.data = [](const void* u, u32 r, u32 c, std::string& scratch) {
        const auto* p = static_cast<const SortFilterProxy*>(u);
        return p->m_src->data(p->m_src->user, p->sourceRow(r), c, scratch);
    };
    if (source && source->header)
        m_model.header = [](const void* u, u32 c) {
            const auto* p = static_cast<const SortFilterProxy*>(u);
            return p->m_src->header(p->m_src->user, c);
        };
}
u32 SortFilterProxy::rows() const {
    if (!m_src || !m_src->rows) return 0;
    return m_identity ? m_src->rows(m_src->user) : u32(m_map.size());
}
void SortFilterProxy::setFilter(u32 col, std::string_view needle) {
    m_filterCol = col;
    m_needle.assign(needle);
    for (char& c : m_needle) c = char(std::tolower(static_cast<unsigned char>(c)));
    invalidate();
}
void SortFilterProxy::sortBy(i32 col, bool ascending) {
    m_sortCol = col;
    m_asc = ascending;
    invalidate();
}
void SortFilterProxy::invalidate() {
    m_identity = m_needle.empty() && m_sortCol < 0;
    m_map.clear();
    if (m_identity || !m_src || !m_src->rows) { m_map.shrink_to_fit(); return; }
    const u32 n = m_src->rows(m_src->user);
    std::string scratch, low;
    m_map.reserve(m_needle.empty() ? n : n / 4);
    for (u32 r = 0; r < n; ++r) {
        if (!m_needle.empty()) {
            const std::string_view d = m_src->data(m_src->user, r, m_filterCol, scratch);
            low.assign(d);
            for (char& c : low) c = char(std::tolower(static_cast<unsigned char>(c)));
            if (low.find(m_needle) == std::string::npos) continue;
        }
        m_map.push_back(r);
    }
    if (m_sortCol >= 0 && u32(m_sortCol) < m_src->columns) {
        // sort keys extracted once: numeric when every key parses, else strings
        const u32 col = u32(m_sortCol);
        std::vector<double> num(m_map.size());
        bool numeric = true;
        for (std::size_t i = 0; i < m_map.size() && numeric; ++i) numeric = parseNum(m_src->data(m_src->user, m_map[i], col, scratch), num[i]);
        std::vector<u32> idx(m_map.size());
        std::iota(idx.begin(), idx.end(), 0u);
        if (numeric) {
            std::stable_sort(idx.begin(), idx.end(), [&](u32 a, u32 b) { return m_asc ? num[a] < num[b] : num[b] < num[a]; });
        } else {
            std::vector<std::string> keys(m_map.size());
            for (std::size_t i = 0; i < m_map.size(); ++i) keys[i].assign(m_src->data(m_src->user, m_map[i], col, scratch));
            std::stable_sort(idx.begin(), idx.end(), [&](u32 a, u32 b) { return m_asc ? keys[a] < keys[b] : keys[b] < keys[a]; });
        }
        std::vector<u32> sorted(m_map.size());
        for (std::size_t i = 0; i < idx.size(); ++i) sorted[i] = m_map[idx[i]];
        m_map = std::move(sorted);
    }
}

// ---- grid view API -----------------------------------------------------------------------------------------------------
NodeHandle gridView(UiContext& ui, NodeHandle parent, const ItemModel* model, f32 tileW, f32 tileH, std::string_view id) {
    registerTierCD(ui);
    const NodeHandle n = make(ui, "gridview", parent, id);
    if (!ui.alive(n)) return n;
    GridState& s = gridPool().create(ui, n);
    s.model = model;
    s.tileW = std::max(16.f, tileW);
    s.tileH = std::max(16.f, tileH);
    ui.setRange(n, -1.f, 1e9f, 1.f);
    ui.setValue(n, -1.f);
    return n;
}
u32 gridColumns(const UiContext& ui, NodeHandle grid) {
    const GridState* s = gridPool().get(ui, grid);
    return s ? GridView::cols(ui, grid, *s) : 0u;
}
u32 gridFirstVisibleRow(const UiContext& ui, NodeHandle grid) {
    const GridState* s = gridPool().get(ui, grid);
    return s ? s->first : 0u;
}

// ---- property grid API ------------------------------------------------------------------------------------------------
NodeHandle propertyGrid(UiContext& ui, NodeHandle parent, std::string_view id) {
    const NodeHandle g = panel(ui, parent, id);
    ui.addClass(g, "propgrid");
    ui.setA11y(g, A11yRole::List, "Properties");
    return g;
}
u32 propertyCount(const UiContext& ui, NodeHandle grid) {
    u32 n = 0;
    for (NodeHandle c = ui.firstChild(grid); ui.alive(c); c = ui.nextSibling(c)) ++n;
    return n;
}
NodeHandle propertyRow(UiContext& ui, NodeHandle grid, const PropertyDesc& d, std::string_view value) {
    const u32 index = propertyCount(ui, grid);
    if (d.kind == PropKind::Group) {
        const NodeHandle row = toolButton(ui, grid, "- " + std::string(d.name));
        ui.addClass(row, "propgroup");
        ui.setCss(row, "text-align", "start");
        ui.setUserData(row, u64(PropKind::Group) | (1ull << 8)); // bit 8: open
        ui.connect(row, SignalKind::Clicked, [grid, index](UiContext& u, NodeHandle n, const SignalArgs&) {
            setPropertyGroupOpen(u, grid, index, !(u.userData(n) & (1ull << 8)));
        });
        return row;
    }
    const NodeHandle row = panel(ui, grid);
    ui.addClass(row, "proprow");
    ui.setUserData(row, u64(d.kind));
    const NodeHandle name = label(ui, row, d.name);
    ui.addClass(name, "propname");
    double num = 0.0;
    NodeHandle ed;
    switch (d.kind) {
    case PropKind::Bool:
        ed = checkBox(ui, row, "", value == "true" || value == "1");
        break;
    case PropKind::Number:
        if (!parseNum(value, num)) num = d.lo;
        ed = spinBox(ui, row, d.lo, d.hi, f32(num), d.step);
        break;
    case PropKind::Enum:
        if (!parseNum(value, num)) num = 0.0;
        ed = comboBox(ui, row, d.options, u32(std::max(0.0, num)));
        break;
    case PropKind::Color: {
        const NodeHandle sw = panel(ui, row);
        ui.setCss(sw, "width", "18px");
        ui.setCss(sw, "height", "18px");
        ui.setCss(sw, "border-radius", "3px");
        ui.setCss(sw, "border", "1px #474955");
        ui.setCss(sw, "background", value.empty() ? "#000000" : value);
        ed = lineEdit(ui, row, value);
        ui.connect(ed, SignalKind::TextChanged, [sw](UiContext& u, NodeHandle, const SignalArgs& a) { u.setCss(sw, "background", a.text); });
        break;
    }
    case PropKind::Text:
    default:
        ed = lineEdit(ui, row, value);
        break;
    }
    ui.setCss(ed, "flex-grow", "1");
    const SignalKind k = d.kind == PropKind::Bool ? SignalKind::Toggled : d.kind == PropKind::Number || d.kind == PropKind::Enum ? SignalKind::ValueChanged : SignalKind::TextChanged;
    ui.connect(ed, k, [grid, index, name](UiContext& u, NodeHandle, const SignalArgs&) {
        u.emit(grid, SignalKind::Submitted, {.value = f32(index), .text = u.text(name)});
    });
    return ed;
}
std::string propertyValue(const UiContext& ui, NodeHandle grid, u32 index) {
    const NodeHandle row = nthChild(ui, grid, index);
    if (!ui.alive(row)) return {};
    const PropKind k = PropKind(ui.userData(row) & 0xFF);
    if (k == PropKind::Group) return {};
    NodeHandle ed = nthChild(ui, row, k == PropKind::Color ? 2 : 1);
    char b[32];
    switch (k) {
    case PropKind::Bool: return ui.checked(ed) ? "true" : "false";
    case PropKind::Number: std::snprintf(b, sizeof b, "%g", double(ui.value(ed))); return b;
    case PropKind::Enum: return std::to_string(i32(ui.value(ed)));
    default: return std::string(ui.text(ed));
    }
}
void setPropertyGroupOpen(UiContext& ui, NodeHandle grid, u32 groupIndex, bool open) {
    const NodeHandle g = nthChild(ui, grid, groupIndex);
    if (!ui.alive(g) || PropKind(ui.userData(g) & 0xFF) != PropKind::Group) return;
    ui.setUserData(g, u64(PropKind::Group) | (open ? (1ull << 8) : 0ull));
    std::string t(ui.text(g));
    if (t.size() > 1) t[0] = open ? '-' : '+';
    ui.setText(g, t);
    for (NodeHandle c = ui.nextSibling(g); ui.alive(c) && PropKind(ui.userData(c) & 0xFF) != PropKind::Group; c = ui.nextSibling(c))
        ui.setCss(c, "display", open ? "flex" : "none");
}

// ---- colour picker API --------------------------------------------------------------------------------------------------
NodeHandle colorPicker(UiContext& ui, NodeHandle parent, Color initial, std::string_view id) {
    registerTierCD(ui);
    const NodeHandle n = make(ui, "colorpicker", parent, id);
    if (!ui.alive(n)) return n;
    ui.setRange(n, 0.f, 360.f, 0.f);
    setColorPickerColor(ui, n, initial);
    return n;
}
Color colorPickerColor(const UiContext& ui, NodeHandle picker) {
    const Hsva h = unpack(ui.userData(picker));
    return hsvToRgb(h.h, h.s, h.v, h.a);
}
void setColorPickerColor(UiContext& ui, NodeHandle picker, Color c) {
    Hsva h = rgbToHsv(c);
    h.h = std::min(h.h, 0.9999f);
    ui.setUserData(picker, pack(h));
    char b[16];
    auto q = [](f32 x) { return unsigned(std::lround(std::clamp(x, 0.f, 1.f) * 255.f)); };
    std::snprintf(b, sizeof b, "#%02X%02X%02X%02X", q(c.r), q(c.g), q(c.b), q(c.a));
    ui.setText(picker, b);
    ui.setValue(picker, h.h * 360.f);
    ui.invalidate(picker);
}

// ---- calendar API -------------------------------------------------------------------------------------------------------
NodeHandle calendar(UiContext& ui, NodeHandle parent, int year, int month, int day, std::string_view id) {
    registerTierCD(ui);
    const NodeHandle n = make(ui, "calendar", parent, id);
    if (!ui.alive(n)) return n;
    ui.setRange(n, -1e7f, 1e7f, 1.f);
    month = std::clamp(month, 1, 12);
    day = std::clamp(day, 1, daysIn(year, month));
    CalendarW::setShown(ui, n, year * 12 + month - 1);
    ui.setValue(n, f32(daysFromCivil(year, month, day)));
    return n;
}
void calendarDate(const UiContext& ui, NodeHandle cal, int& year, int& month, int& day) {
    civilFromDays(i32(ui.value(cal)), year, month, day);
}

// ---- chart API ------------------------------------------------------------------------------------------------------------
NodeHandle chart(UiContext& ui, NodeHandle parent, ChartKind kind, std::string_view id) {
    registerTierCD(ui);
    const NodeHandle n = make(ui, "chart", parent, id);
    if (!ui.alive(n)) return n;
    chartPool().create(ui, n).kind = kind;
    ui.setRange(n, -1.f, 1e9f, 1.f);
    ui.setValue(n, -1.f);
    return n;
}
void chartSetSeries(UiContext& ui, NodeHandle c, u32 index, std::string_view name, std::span<const f32> values, Color color) {
    ChartState* s = chartPool().get(ui, c);
    if (!s || index > 64) return;
    if (s->series.size() <= index) s->series.resize(index + 1);
    s->series[index] = {std::string(name), std::vector<f32>(values.begin(), values.end()), color};
    ui.invalidate(c);
}
u32 chartSeriesCount(const UiContext& ui, NodeHandle c) {
    const ChartState* s = chartPool().get(ui, c);
    return s ? u32(s->series.size()) : 0u;
}

// ---- markdown view --------------------------------------------------------------------------------------------------------
NodeHandle markdownView(UiContext& ui, NodeHandle parent, std::string_view md, std::string_view id) {
    const NodeHandle v = panel(ui, parent, id);
    ui.addClass(v, "md");
    ui.setA11y(v, A11yRole::Group, "Document");
    setMarkdown(ui, v, md);
    return v;
}
void setMarkdown(UiContext& ui, NodeHandle view, std::string_view md) {
    std::vector<NodeHandle> old;
    for (NodeHandle c = ui.firstChild(view); ui.alive(c); c = ui.nextSibling(c)) old.push_back(c);
    for (NodeHandle c : old) ui.destroy(c);
    std::string para, code;
    bool inCode = false;
    auto emit = [&](std::string text, std::string_view cls) {
        if (cls != "md-code") stripInline(text);
        const NodeHandle l = label(ui, view, text);
        ui.addClass(l, cls);
    };
    auto flush = [&] { if (!para.empty()) { emit(para, "md-p"); para.clear(); } };
    std::size_t at = 0;
    while (at <= md.size()) {
        std::size_t nl = md.find('\n', at);
        if (nl == std::string_view::npos) nl = md.size();
        std::string_view line = md.substr(at, nl - at);
        at = nl + 1;
        if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
        if (line.starts_with("```")) {
            if (inCode) { if (!code.empty() && code.back() == '\n') code.pop_back(); emit(code, "md-code"); code.clear(); }
            else flush();
            inCode = !inCode;
            continue;
        }
        if (inCode) { code.append(line); code += '\n'; continue; }
        std::size_t lead = line.find_first_not_of(' ');
        if (lead == std::string_view::npos) { flush(); continue; }
        line.remove_prefix(lead);
        if (line.starts_with("### ")) { flush(); emit(std::string(line.substr(4)), "md-h3"); }
        else if (line.starts_with("## ")) { flush(); emit(std::string(line.substr(3)), "md-h2"); }
        else if (line.starts_with("# ")) { flush(); emit(std::string(line.substr(2)), "md-h1"); }
        else if (line.starts_with("- ") || line.starts_with("* ")) { flush(); emit("\xE2\x80\xA2 " + std::string(line.substr(2)), "md-li"); }
        else if (line.starts_with("> ")) { flush(); emit(std::string(line.substr(2)), "md-quote"); }
        else if (std::size_t d = line.find(". "); d != std::string_view::npos && d > 0 && d < 4 &&
                 line.substr(0, d).find_first_not_of("0123456789") == std::string_view::npos) { flush(); emit(std::string(line), "md-li"); }
        else { if (!para.empty()) para += ' '; para.append(line); }
        if (nl == md.size()) break;
    }
    if (inCode && !code.empty()) emit(code, "md-code");
    flush();
}

} // namespace fuse::ui::widgets
