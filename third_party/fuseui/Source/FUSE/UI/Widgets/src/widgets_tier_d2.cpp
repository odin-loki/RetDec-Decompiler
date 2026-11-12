// FUSE UI Tier D (2) widgets (plan §5): font picker, file dialog, node graph, curve editor, timeline, viewport,
// code editor, date-time edit and a video widget over a pluggable decoder (stub: test pattern).
#include "fuse/ui/widgets/widgets.hpp"
#include "widgets_internal.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <memory>
#include <string>
#include <system_error>
#include <vector>

namespace fuse::ui::widgets {
namespace {

NodeHandle make(UiContext& ui, std::string_view cls, NodeHandle parent, std::string_view id = {}) {
    auto h = ui.create(cls, parent, id);
    return h ? *h : NodeHandle{};
}
Color accentOf(const PaintContext& pc) { return pc.tint(pc.style.background2.a > 0.f ? pc.style.background2 : Color::rgb(0x3D8BFD)); }
bool inside(Rect r, Vec2 p) { return p.x >= r.x && p.y >= r.y && p.x < r.right() && p.y < r.bottom(); }
NodeHandle hbox(UiContext& ui, NodeHandle parent) {
    const NodeHandle r = panel(ui, parent);
    ui.addClass(r, "row");
    return r;
}

/// Per-node state pool keyed by user data (slot + 1); see widgets_tier_cd.cpp.
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

void bezier(DrawList& dl, Vec2 a, Vec2 b, Color c, f32 w) {
    const f32 dx = std::max(40.f, std::abs(b.x - a.x) * 0.5f);
    const Vec2 c1{a.x + dx, a.y}, c2{b.x - dx, b.y};
    Vec2 prev = a;
    constexpr int kSeg = 20;
    for (int i = 1; i <= kSeg; ++i) {
        const f32 t = f32(i) / kSeg, u = 1.f - t;
        const Vec2 p{u * u * u * a.x + 3 * u * u * t * c1.x + 3 * u * t * t * c2.x + t * t * t * b.x,
                     u * u * u * a.y + 3 * u * u * t * c1.y + 3 * u * t * t * c2.y + t * t * t * b.y};
        dl.line(prev, p, c, w);
        prev = p;
    }
}
void dot(DrawList& dl, Vec2 p, f32 r, Color fill, Color ring = {}) {
    dl.roundedRect({p.x - r, p.y - r, 2 * r, 2 * r}, fill, {r, r, r, r}, ring, ring.a > 0.f ? 1.5f : 0.f);
}

// =====================================================================================================================
// Font picker
struct FontState { NodeHandle list, size, preview; std::string family; };
Pool<FontState>& fontPool() { static Pool<FontState> p; return p; }
void fontRefresh(UiContext& ui, NodeHandle picker, bool emit) {
    FontState* s = fontPool().get(ui, picker);
    if (!s) return;
    const i32 sel = i32(ui.value(s->list));
    s->family = sel >= 0 ? std::string(listItem(ui, s->list, u32(sel))) : std::string();
    const f32 px = ui.value(s->size);
    char b[32];
    std::snprintf(b, sizeof b, "%gpx", double(px));
    ui.setCss(s->preview, "font-size", b);
    ui.setText(s->preview, s->family + " - The quick brown fox jumps over the lazy dog 0123");
    if (emit) ui.emit(picker, SignalKind::Submitted, {.value = px, .text = s->family});
}

// =====================================================================================================================
// File dialog
struct FileDlgState {
    std::string dir, accepted;
    std::vector<std::string> filters, entries; // entries: folders end in '/'
    bool save = false;
    NodeHandle path, list, name;
};
Pool<FileDlgState>& filePool() { static Pool<FileDlgState> p; return p; }
bool globMatch(std::string_view pat, std::string_view s) {
    std::size_t p = 0, i = 0, star = std::string_view::npos, mark = 0;
    auto low = [](char c) { return char(c >= 'A' && c <= 'Z' ? c + 32 : c); };
    while (i < s.size()) {
        if (p < pat.size() && (pat[p] == '?' || low(pat[p]) == low(s[i]))) { ++p; ++i; }
        else if (p < pat.size() && pat[p] == '*') { star = p++; mark = i; }
        else if (star != std::string_view::npos) { p = star + 1; i = ++mark; }
        else return false;
    }
    while (p < pat.size() && pat[p] == '*') ++p;
    return p == pat.size();
}
std::string joinPath(std::string_view dir, std::string_view name) {
    std::filesystem::path p{std::string(dir)};
    p /= std::string(name);
    return p.lexically_normal().generic_string();
}
bool fileAccept(UiContext& ui, NodeHandle dlg, FileDlgState& s, std::string_view name) {
    if (name.empty()) return false;
    const std::string full = joinPath(s.dir, name);
    std::error_code ec;
    if (std::filesystem::is_directory(full, ec)) return fileDialogNavigate(ui, dlg, full);
    if (!s.save && !std::filesystem::exists(full, ec)) return false;
    s.accepted = full;
    closeDialog(ui, dlg);
    ui.emit(dlg, SignalKind::Submitted, {.text = s.accepted});
    return true;
}

// =====================================================================================================================
// Node graph
struct GNode {
    std::string title;
    Vec2 pos;
    std::vector<std::pair<std::string, u32>> in, out;
    bool alive = true;
};
struct GLink { u32 from, outPin, to, inPin; };
struct GraphState {
    std::vector<GNode> nodes;
    std::vector<GLink> links;
    Vec2 pan{0.f, 0.f};
    f32 zoom = 1.f;
    u8 drag = 0; // 1 node, 2 link, 3 pan
    u32 dragNode = 0, dragPin = 0;
    Vec2 grab, cursor;
};
Pool<GraphState>& graphPool() { static Pool<GraphState> p; return p; }
constexpr f32 kGW = 150.f, kGHead = 22.f, kGRow = 18.f;
Rect gnodeRect(const UiContext& ui, NodeHandle n, const GraphState& s, u32 i) {
    const Rect c = ui.contentRect(n);
    const GNode& g = s.nodes[i];
    const f32 rows = f32(std::max(g.in.size(), g.out.size()));
    return {c.x + (g.pos.x + s.pan.x) * s.zoom, c.y + (g.pos.y + s.pan.y) * s.zoom, kGW * s.zoom,
            (kGHead + rows * kGRow + 6.f) * s.zoom};
}
Vec2 gpin(const UiContext& ui, NodeHandle n, const GraphState& s, u32 i, u32 pin, bool out) {
    const Rect r = gnodeRect(ui, n, s, i);
    return {out ? r.right() : r.x, r.y + (kGHead + (f32(pin) + 0.5f) * kGRow) * s.zoom};
}
bool reaches(const GraphState& s, u32 from, u32 target) { // is there a path from -> ... -> target?
    std::vector<u32> stack{from};
    std::vector<u8> seen(s.nodes.size(), 0);
    while (!stack.empty()) {
        const u32 k = stack.back();
        stack.pop_back();
        if (k == target) return true;
        if (seen[k]) continue;
        seen[k] = 1;
        for (const GLink& l : s.links) if (l.from == k) stack.push_back(l.to);
    }
    return false;
}
bool gconnect(GraphState& s, u32 a, u32 op, u32 b, u32 ip) {
    if (a >= s.nodes.size() || b >= s.nodes.size() || a == b || !s.nodes[a].alive || !s.nodes[b].alive) return false;
    if (op >= s.nodes[a].out.size() || ip >= s.nodes[b].in.size()) return false;
    const u32 ta = s.nodes[a].out[op].second, tb = s.nodes[b].in[ip].second;
    if (ta && tb && ta != tb) return false;
    if (reaches(s, b, a)) return false; // would close a cycle
    std::erase_if(s.links, [&](const GLink& l) { return l.to == b && l.inPin == ip; });
    s.links.push_back({a, op, b, ip});
    return true;
}
Color pinColor(u32 type) {
    static const u32 pal[] = {0xB0B0B8, 0x6BCB77, 0x4D96FF, 0xFFD93D, 0xFF6B6B, 0xC77DFF};
    return Color::rgb(pal[type % 6]);
}

struct NodeGraphW {
    static constexpr std::string_view kName = "nodegraph";
    static constexpr A11yRole kRole = A11yRole::Group;
    static constexpr bool kFocusable = true;
    static constexpr bool kInteractive = true;
    static Size measure(UiContext&, NodeHandle, Size) { return {480.f, 320.f}; }
    static void paint(const PaintContext& pc, DrawList& dl) {
        UiContext& ui = pc.ui;
        GraphState* s = graphPool().get(ui, pc.node);
        if (!s) return;
        const Rect c = pc.content;
        dl.pushClip(c);
        dl.rect(c, pc.tint(Color::rgb(0x17171C)));
        const f32 step = 24.f * s->zoom;
        if (step >= 6.f) {
            const f32 ox = std::fmod(s->pan.x * s->zoom, step), oy = std::fmod(s->pan.y * s->zoom, step);
            for (f32 x = c.x + (ox < 0 ? ox + step : ox); x < c.right(); x += step) dl.line({x, c.y}, {x, c.bottom()}, pc.tint(Color::rgb(0x23232A)), 1.f);
            for (f32 y = c.y + (oy < 0 ? oy + step : oy); y < c.bottom(); y += step) dl.line({c.x, y}, {c.right(), y}, pc.tint(Color::rgb(0x23232A)), 1.f);
        }
        for (const GLink& l : s->links)
            bezier(dl, gpin(ui, pc.node, *s, l.from, l.outPin, true), gpin(ui, pc.node, *s, l.to, l.inPin, false),
                   pc.tint(pinColor(s->nodes[l.from].out[l.outPin].second)), 2.f * s->zoom);
        if (s->drag == 2)
            bezier(dl, gpin(ui, pc.node, *s, s->dragNode, s->dragPin, true), s->cursor, pc.tint(Color::rgb(0xFFFFFF, 0.7f)), 2.f);
        const i32 sel = i32(ui.value(pc.node));
        const Color text = pc.tint(pc.style.color), acc = accentOf(pc);
        const f32 fs = pc.style.fontSize * 0.86f * s->zoom;
        for (u32 i = 0; i < s->nodes.size(); ++i) {
            const GNode& g = s->nodes[i];
            if (!g.alive) continue;
            const Rect r = gnodeRect(ui, pc.node, *s, i);
            const f32 rad = 6.f * s->zoom;
            dl.shadow(r, Color::rgb(0x000000, 0.5f * pc.opacity), 10.f, rad);
            dl.roundedRect(r, pc.tint(Color::rgb(0x2A2A33)), {rad, rad, rad, rad}, i32(i) == sel ? acc : pc.tint(Color::rgb(0x3A3A45)),
                           i32(i) == sel ? 2.f : 1.f);
            dl.roundedRect({r.x, r.y, r.w, kGHead * s->zoom}, pc.tint(Color::rgb(0x3B4A6B)), {rad, rad, 0.f, 0.f});
            ui.paintString(dl, g.title, {r.x + 8.f * s->zoom, r.y, r.w - 16.f * s->zoom, kGHead * s->zoom}, text, fs,
                           TextAlignment::Start, pc.node.index);
            for (u32 k = 0; k < g.in.size(); ++k) {
                const Vec2 p = gpin(ui, pc.node, *s, i, k, false);
                dot(dl, p, 4.5f * s->zoom, pc.tint(pinColor(g.in[k].second)), pc.tint(Color::rgb(0x111111)));
                ui.paintString(dl, g.in[k].first, {p.x + 9.f * s->zoom, p.y - kGRow * 0.5f * s->zoom, r.w * 0.5f, kGRow * s->zoom}, text,
                               fs * 0.92f, TextAlignment::Start, pc.node.index);
            }
            for (u32 k = 0; k < g.out.size(); ++k) {
                const Vec2 p = gpin(ui, pc.node, *s, i, k, true);
                dot(dl, p, 4.5f * s->zoom, pc.tint(pinColor(g.out[k].second)), pc.tint(Color::rgb(0x111111)));
                ui.paintString(dl, g.out[k].first, {r.x + r.w * 0.5f, p.y - kGRow * 0.5f * s->zoom, r.w * 0.5f - 9.f * s->zoom, kGRow * s->zoom},
                               text, fs * 0.92f, TextAlignment::End, pc.node.index);
            }
        }
        dl.popClip();
    }
    static bool hitPin(const UiContext& ui, NodeHandle n, const GraphState& s, Vec2 p, bool out, u32& node, u32& pin) {
        const f32 r2 = 9.f * 9.f * std::max(1.f, s.zoom * s.zoom);
        for (u32 i = u32(s.nodes.size()); i-- > 0;) {
            if (!s.nodes[i].alive) continue;
            const u32 cnt = u32(out ? s.nodes[i].out.size() : s.nodes[i].in.size());
            for (u32 k = 0; k < cnt; ++k) {
                const Vec2 q = gpin(ui, n, s, i, k, out);
                if ((q.x - p.x) * (q.x - p.x) + (q.y - p.y) * (q.y - p.y) <= r2) { node = i; pin = k; return true; }
            }
        }
        return false;
    }
    static bool onEvent(EventContext& ec, const Event& e) {
        if (ec.phase != Phase::Target) return false;
        UiContext& ui = ec.ui;
        GraphState* s = graphPool().get(ui, ec.node);
        if (!s) return false;
        switch (e.type) {
        case EventType::PointerDown: {
            u32 nd = 0, pin = 0;
            if (e.button == MouseButton::Left && hitPin(ui, ec.node, *s, e.pos, true, nd, pin)) {
                s->drag = 2; s->dragNode = nd; s->dragPin = pin; s->cursor = e.pos;
                return true;
            }
            if (e.button == MouseButton::Left && hitPin(ui, ec.node, *s, e.pos, false, nd, pin)) { // pull a link off an input
                for (const GLink& l : s->links)
                    if (l.to == nd && l.inPin == pin) {
                        s->drag = 2; s->dragNode = l.from; s->dragPin = l.outPin; s->cursor = e.pos;
                        nodeGraphDisconnect(ui, ec.node, nd, pin);
                        return true;
                    }
            }
            for (u32 i = u32(s->nodes.size()); i-- > 0;)
                if (s->nodes[i].alive && inside(gnodeRect(ui, ec.node, *s, i), e.pos) && e.button == MouseButton::Left) {
                    s->drag = 1; s->dragNode = i;
                    s->grab = {e.pos.x / s->zoom - s->nodes[i].pos.x, e.pos.y / s->zoom - s->nodes[i].pos.y};
                    ui.setValue(ec.node, f32(i), true);
                    ui.invalidate(ec.node);
                    return true;
                }
            s->drag = 3; s->grab = e.pos;
            if (e.button == MouseButton::Left) ui.setValue(ec.node, -1.f, true);
            ui.invalidate(ec.node);
            return true;
        }
        case EventType::PointerMove:
            if (s->drag == 1) s->nodes[s->dragNode].pos = {e.pos.x / s->zoom - s->grab.x, e.pos.y / s->zoom - s->grab.y};
            else if (s->drag == 2) s->cursor = e.pos;
            else if (s->drag == 3) { s->pan.x += (e.pos.x - s->grab.x) / s->zoom; s->pan.y += (e.pos.y - s->grab.y) / s->zoom; s->grab = e.pos; }
            else return false;
            ui.invalidate(ec.node);
            return true;
        case EventType::PointerUp: {
            if (s->drag == 2) {
                u32 nd = 0, pin = 0;
                if (hitPin(ui, ec.node, *s, e.pos, false, nd, pin)) nodeGraphConnect(ui, ec.node, s->dragNode, s->dragPin, nd, pin);
            }
            s->drag = 0;
            ui.invalidate(ec.node);
            return true;
        }
        case EventType::Wheel: {
            const Rect c = ui.contentRect(ec.node);
            const f32 nz = std::clamp(s->zoom * (e.wheel.y > 0 ? 1.1f : 1.f / 1.1f), 0.25f, 3.f);
            // keep the graph point under the pointer fixed
            const Vec2 g{(e.pos.x - c.x) / s->zoom - s->pan.x, (e.pos.y - c.y) / s->zoom - s->pan.y};
            s->zoom = nz;
            s->pan = {(e.pos.x - c.x) / nz - g.x, (e.pos.y - c.y) / nz - g.y};
            ui.invalidate(ec.node);
            return true;
        }
        case EventType::KeyDown:
            if (e.key == Key::Delete && ui.value(ec.node) >= 0.f) {
                nodeGraphRemoveNode(ui, ec.node, u32(ui.value(ec.node)));
                return true;
            }
            return false;
        default: return false;
        }
    }
};

// =====================================================================================================================
// Curve editor
struct CKey { f32 t, v; CurveInterp interp; };
struct CurveState {
    std::vector<CKey> keys;
    f32 t0 = 0.f, t1 = 1.f, v0 = 0.f, v1 = 1.f;
    i32 drag = -1;
};
Pool<CurveState>& curvePool() { static Pool<CurveState> p; return p; }
f32 curveEval(const CurveState& s, f32 t) {
    const auto& k = s.keys;
    if (k.empty()) return 0.f;
    if (t <= k.front().t) return k.front().v;
    if (t >= k.back().t) return k.back().v;
    std::size_t i = 0;
    while (i + 1 < k.size() && k[i + 1].t < t) ++i;
    const CKey &a = k[i], &b = k[i + 1];
    const f32 h = b.t - a.t;
    if (h <= 0.f) return b.v;
    const f32 u = (t - a.t) / h;
    switch (a.interp) {
    case CurveInterp::Step: return a.v;
    case CurveInterp::Linear: return a.v + (b.v - a.v) * u;
    case CurveInterp::Smooth: break;
    }
    auto slope = [&](std::size_t j) {
        if (j == 0) return (k[1].v - k[0].v) / std::max(1e-6f, k[1].t - k[0].t);
        if (j + 1 == k.size()) return (k[j].v - k[j - 1].v) / std::max(1e-6f, k[j].t - k[j - 1].t);
        return (k[j + 1].v - k[j - 1].v) / std::max(1e-6f, k[j + 1].t - k[j - 1].t);
    };
    const f32 m0 = slope(i) * h, m1 = slope(i + 1) * h, u2 = u * u, u3 = u2 * u;
    return (2 * u3 - 3 * u2 + 1) * a.v + (u3 - 2 * u2 + u) * m0 + (-2 * u3 + 3 * u2) * b.v + (u3 - u2) * m1;
}
struct CurveW {
    static constexpr std::string_view kName = "curveeditor";
    static constexpr A11yRole kRole = A11yRole::Group;
    static constexpr bool kFocusable = true;
    static constexpr bool kInteractive = true;
    static Size measure(UiContext&, NodeHandle, Size) { return {360.f, 200.f}; }
    static Vec2 toScreen(Rect c, const CurveState& s, f32 t, f32 v) {
        return {c.x + (t - s.t0) / (s.t1 - s.t0) * c.w, c.bottom() - (v - s.v0) / (s.v1 - s.v0) * c.h};
    }
    static void paint(const PaintContext& pc, DrawList& dl) {
        CurveState* s = curvePool().get(pc.ui, pc.node);
        if (!s) return;
        const Rect c = pc.content;
        dl.pushClip(c);
        dl.rect(c, pc.tint(Color::rgb(0x17171C)), 4.f);
        for (int i = 1; i < 4; ++i) {
            dl.line({c.x + c.w * f32(i) / 4.f, c.y}, {c.x + c.w * f32(i) / 4.f, c.bottom()}, pc.tint(Color::rgb(0x2A2A33)), 1.f);
            dl.line({c.x, c.y + c.h * f32(i) / 4.f}, {c.right(), c.y + c.h * f32(i) / 4.f}, pc.tint(Color::rgb(0x2A2A33)), 1.f);
        }
        const Color acc = accentOf(pc);
        Vec2 prev = toScreen(c, *s, s->t0, curveEval(*s, s->t0));
        for (f32 x = 2.f; x <= c.w + 1.f; x += 2.f) {
            const f32 t = s->t0 + (s->t1 - s->t0) * std::min(1.f, x / c.w);
            const Vec2 p = toScreen(c, *s, t, curveEval(*s, t));
            dl.line(prev, p, acc, 2.f);
            prev = p;
        }
        const i32 sel = i32(pc.ui.value(pc.node));
        for (u32 i = 0; i < s->keys.size(); ++i) {
            const Vec2 p = toScreen(c, *s, s->keys[i].t, s->keys[i].v);
            dl.rect({p.x - 4.f, p.y - 4.f, 8.f, 8.f}, pc.tint(i32(i) == sel ? Color::rgb(0xFFD93D) : Color::rgb(0xE8E8EE)), 1.f);
        }
        dl.popClip();
    }
    static void changed(UiContext& ui, NodeHandle n, const CurveState& s) {
        ui.invalidate(n);
        ui.emit(n, SignalKind::ValueChanged, {.value = f32(s.keys.size())});
    }
    static bool onEvent(EventContext& ec, const Event& e) {
        if (ec.phase != Phase::Target) return false;
        UiContext& ui = ec.ui;
        CurveState* s = curvePool().get(ui, ec.node);
        if (!s) return false;
        const Rect c = ui.contentRect(ec.node);
        auto fromScreen = [&](Vec2 p, f32& t, f32& v) {
            t = s->t0 + std::clamp((p.x - c.x) / c.w, 0.f, 1.f) * (s->t1 - s->t0);
            v = s->v0 + std::clamp((c.bottom() - p.y) / c.h, 0.f, 1.f) * (s->v1 - s->v0);
        };
        switch (e.type) {
        case EventType::PointerDown: {
            for (u32 i = 0; i < s->keys.size(); ++i) {
                const Vec2 p = toScreen(c, *s, s->keys[i].t, s->keys[i].v);
                if (std::abs(p.x - e.pos.x) <= 6.f && std::abs(p.y - e.pos.y) <= 6.f) {
                    s->drag = i32(i);
                    ui.setValue(ec.node, f32(i));
                    ui.invalidate(ec.node);
                    return true;
                }
            }
            f32 t, v;
            fromScreen(e.pos, t, v);
            s->drag = i32(curveAddKey(ui, ec.node, t, v));
            ui.setValue(ec.node, f32(s->drag));
            return true;
        }
        case EventType::PointerMove: {
            if (s->drag < 0) return false;
            f32 t, v;
            fromScreen(e.pos, t, v);
            const std::size_t i = std::size_t(s->drag);
            const f32 lo = i > 0 ? s->keys[i - 1].t + 1e-4f : s->t0, hi = i + 1 < s->keys.size() ? s->keys[i + 1].t - 1e-4f : s->t1;
            s->keys[i].t = std::clamp(t, lo, hi);
            s->keys[i].v = v;
            changed(ui, ec.node, *s);
            return true;
        }
        case EventType::PointerUp: s->drag = -1; return true;
        case EventType::KeyDown:
            if ((e.key == Key::Delete || e.key == Key::Backspace) && ui.value(ec.node) >= 0.f && ui.value(ec.node) < f32(s->keys.size())) {
                s->keys.erase(s->keys.begin() + i32(ui.value(ec.node)));
                ui.setValue(ec.node, -1.f);
                changed(ui, ec.node, *s);
                return true;
            }
            return false;
        default: return false;
        }
    }
};

// =====================================================================================================================
// Timeline
struct Clip { f32 start, end; std::string label; Color color; };
struct Track { std::string name; std::vector<Clip> clips; };
struct TimelineState {
    std::vector<Track> tracks;
    f32 duration = 10.f, pxPerSec = 60.f, scrollSec = 0.f, snap = 0.f;
    f32 viewStart = 0.f, viewSpan = 0.f; ///< explicit view (timelineSetView); span 0 = free pxPerSec / scrollSec
    u8 drag = 0; // 1 scrub, 2 clip
    u32 dTrack = 0, dClip = 0;
    f32 grabSec = 0.f;
};
Pool<TimelineState>& tlPool() { static Pool<TimelineState> p; return p; }
constexpr f32 kTlHead = 96.f, kTlRuler = 22.f, kTlRow = 28.f;
struct TimelineW {
    static constexpr std::string_view kName = "timeline";
    static constexpr A11yRole kRole = A11yRole::Slider;
    static constexpr bool kFocusable = true;
    static constexpr bool kInteractive = true;
    static Size measure(UiContext& ui, NodeHandle n, Size) {
        const TimelineState* s = tlPool().get(ui, n);
        return {480.f, kTlRuler + f32(s ? s->tracks.size() : 0) * kTlRow + 4.f};
    }
    /// An explicit view range fixes the scale to the lane width.
    static void applyView(Rect c, TimelineState& s) {
        if (s.viewSpan <= 0.f) return;
        s.pxPerSec = std::max(1.f, c.w - kTlHead) / s.viewSpan;
        s.scrollSec = s.viewStart;
    }
    static void setView(TimelineState& s, f32 start, f32 span) {
        span = std::clamp(span, std::min(0.05f, s.duration), s.duration);
        s.viewSpan = span;
        s.viewStart = std::clamp(start, 0.f, std::max(0.f, s.duration - span));
    }
    static f32 xOf(Rect c, const TimelineState& s, f32 t) { return c.x + kTlHead + (t - s.scrollSec) * s.pxPerSec; }
    static f32 tOf(Rect c, const TimelineState& s, f32 x) { return (x - c.x - kTlHead) / s.pxPerSec + s.scrollSec; }
    static void paint(const PaintContext& pc, DrawList& dl) {
        UiContext& ui = pc.ui;
        TimelineState* s = tlPool().get(ui, pc.node);
        if (!s) return;
        const Rect c = pc.content;
        applyView(c, *s);
        const Color text = pc.tint(pc.style.color), dim = pc.tint(Color::rgb(0x8A8A96));
        const f32 fs = pc.style.fontSize * 0.8f;
        dl.pushClip(c);
        dl.rect(c, pc.tint(Color::rgb(0x17171C)));
        dl.rect({c.x, c.y, c.w, kTlRuler}, pc.tint(Color::rgb(0x22222A)));
        const Rect lane{c.x + kTlHead, c.y, c.w - kTlHead, c.h};
        dl.pushClip(lane);
        // adaptive ruler (Qt TimelineCanvas): the smallest step that keeps <= 12 labels in view
        const f32 visible = std::max(1.f, c.w - kTlHead) / s->pxPerSec;
        f32 major = 60.f;
        for (const f32 st : {0.01f, 0.02f, 0.05f, 0.1f, 0.25f, 0.5f, 1.f, 2.f, 5.f, 10.f, 30.f, 60.f})
            if (visible / st <= 12.f) { major = st; break; }
        for (f32 t = std::floor(s->scrollSec / major + 1e-4f) * major; t <= s->duration + 1e-4f; t += major) {
            const f32 x = xOf(c, *s, t);
            if (x > c.right()) break;
            dl.line({x, c.y + kTlRuler - 8.f}, {x, c.bottom()}, pc.tint(Color::rgb(0x2A2A33)), 1.f);
            char b[16];
            std::snprintf(b, sizeof b, "%gs", double(t));
            ui.paintString(dl, b, {x + 3.f, c.y, 60.f, kTlRuler - 4.f}, dim, fs, TextAlignment::Start, pc.node.index);
        }
        for (u32 k = 0; k < s->tracks.size(); ++k) {
            const f32 y = c.y + kTlRuler + f32(k) * kTlRow;
            for (const Clip& cl : s->tracks[k].clips) {
                const Rect r{xOf(c, *s, cl.start), y + 3.f, (cl.end - cl.start) * s->pxPerSec, kTlRow - 6.f};
                dl.rect(r, pc.tint(cl.color), 4.f);
                ui.paintString(dl, cl.label, {r.x + 6.f, r.y, std::max(0.f, r.w - 8.f), r.h}, pc.tint(Color::rgb(0x101014)), fs,
                               TextAlignment::Start, pc.node.index);
            }
        }
        const f32 px = xOf(c, *s, ui.value(pc.node));
        dl.line({px, c.y}, {px, c.bottom()}, pc.tint(Color::rgb(0xFF6B6B)), 2.f);
        dl.rect({px - 5.f, c.y, 10.f, 10.f}, pc.tint(Color::rgb(0xFF6B6B)), 2.f);
        dl.popClip();
        for (u32 k = 0; k < s->tracks.size(); ++k) {
            const f32 y = c.y + kTlRuler + f32(k) * kTlRow;
            dl.line({c.x, y + kTlRow}, {c.right(), y + kTlRow}, pc.tint(Color::rgb(0x26262E)), 1.f);
            ui.paintString(dl, s->tracks[k].name, {c.x + 6.f, y, kTlHead - 10.f, kTlRow}, text, fs, TextAlignment::Start, pc.node.index);
        }
        dl.line({c.x + kTlHead, c.y}, {c.x + kTlHead, c.bottom()}, pc.tint(Color::rgb(0x3A3A45)), 1.f);
        dl.popClip();
    }
    static f32 snapT(const TimelineState& s, f32 t) { return s.snap > 0.f ? std::round(t / s.snap) * s.snap : t; }
    static void scrub(UiContext& ui, NodeHandle n, const TimelineState& s, f32 t) {
        ui.setValue(n, std::clamp(t, 0.f, s.duration), true);
        ui.invalidate(n);
    }
    static bool onEvent(EventContext& ec, const Event& e) {
        if (ec.phase != Phase::Target) return false;
        UiContext& ui = ec.ui;
        TimelineState* s = tlPool().get(ui, ec.node);
        if (!s) return false;
        const Rect c = ui.contentRect(ec.node);
        applyView(c, *s);
        switch (e.type) {
        case EventType::PointerDown: {
            if (e.pos.x < c.x + kTlHead) return true;
            const f32 t = tOf(c, *s, e.pos.x);
            if (e.pos.y >= c.y + kTlRuler) {
                const u32 k = u32((e.pos.y - c.y - kTlRuler) / kTlRow);
                if (k < s->tracks.size())
                    for (u32 j = 0; j < s->tracks[k].clips.size(); ++j) {
                        const Clip& cl = s->tracks[k].clips[j];
                        if (t >= cl.start && t < cl.end) { s->drag = 2; s->dTrack = k; s->dClip = j; s->grabSec = t - cl.start; return true; }
                    }
            }
            s->drag = 1;
            scrub(ui, ec.node, *s, snapT(*s, t));
            return true;
        }
        case EventType::PointerMove: {
            const f32 t = tOf(c, *s, e.pos.x);
            if (s->drag == 1) scrub(ui, ec.node, *s, snapT(*s, t));
            else if (s->drag == 2) {
                Clip& cl = s->tracks[s->dTrack].clips[s->dClip];
                const f32 len = cl.end - cl.start;
                cl.start = std::clamp(snapT(*s, t - s->grabSec), 0.f, std::max(0.f, s->duration - len));
                cl.end = cl.start + len;
                ui.invalidate(ec.node);
            } else return false;
            return true;
        }
        case EventType::PointerUp: s->drag = 0; return true;
        case EventType::Wheel: {
            const f32 anchor = tOf(c, *s, e.pos.x);
            if (s->viewSpan > 0.f) {
                // Qt: Ctrl+wheel zooms about the pointer, Shift+wheel (or horizontal) pans; Event {value = 1} tells
                // the owner the view changed (timelineView)
                const f32 d = e.wheel.y != 0.f ? e.wheel.y : e.wheel.x;
                if (d == 0.f) return false;
                if (e.mods & kModCtrl) {
                    const f32 rel = (anchor - s->viewStart) / s->viewSpan, span = s->viewSpan / (d > 0 ? 1.25f : 0.8f);
                    setView(*s, anchor - rel * std::clamp(span, std::min(0.05f, s->duration), s->duration), span);
                } else if ((e.mods & kModShift) || e.wheel.x != 0.f) {
                    setView(*s, s->viewStart - d * s->viewSpan / 10.f, s->viewSpan);
                } else {
                    return false;
                }
                ui.invalidate(ec.node);
                ui.emit(ec.node, SignalKind::Event, {.value = 1.f});
                return true;
            }
            s->pxPerSec = std::clamp(s->pxPerSec * (e.wheel.y > 0 ? 1.2f : 1.f / 1.2f), 4.f, 600.f);
            s->scrollSec = std::max(0.f, anchor - (e.pos.x - c.x - kTlHead) / s->pxPerSec);
            ui.invalidate(ec.node);
            return true;
        }
        case EventType::KeyDown: {
            const f32 st = s->snap > 0.f ? s->snap : 1.f / 30.f;
            if (e.key == Key::Right) { scrub(ui, ec.node, *s, ui.value(ec.node) + st); return true; }
            if (e.key == Key::Left) { scrub(ui, ec.node, *s, ui.value(ec.node) - st); return true; }
            if (e.key == Key::Home) { scrub(ui, ec.node, *s, 0.f); return true; }
            if (e.key == Key::End) { scrub(ui, ec.node, *s, s->duration); return true; }
            return false;
        }
        default: return false;
        }
    }
};

// =====================================================================================================================
// Viewport
struct ViewportState { u32 image = 0, w = 1, h = 1, count = 0; ViewportInput last; ViewportOverlay overlay; };
Pool<ViewportState>& vpPool() { static Pool<ViewportState> p; return p; }
Rect letterbox(Rect c, u32 w, u32 h) {
    const f32 s = std::min(c.w / f32(std::max(1u, w)), c.h / f32(std::max(1u, h)));
    const f32 iw = f32(w) * s, ih = f32(h) * s;
    return {c.x + (c.w - iw) * 0.5f, c.y + (c.h - ih) * 0.5f, iw, ih};
}
struct ViewportW {
    static constexpr std::string_view kName = "viewport";
    static constexpr A11yRole kRole = A11yRole::Image;
    static constexpr bool kFocusable = true;
    static constexpr bool kInteractive = true;
    static Size measure(UiContext&, NodeHandle, Size) { return {320.f, 180.f}; }
    static void paint(const PaintContext& pc, DrawList& dl) {
        ViewportState* s = vpPool().get(pc.ui, pc.node);
        if (!s) return;
        dl.rect(pc.content, pc.tint(Color::rgb(0x000000)));
        DrawCmd c;
        c.op = DrawOp::Image;
        c.image = s->image;
        c.rect = letterbox(pc.content, s->w, s->h);
        c.fill = Color{1.f, 1.f, 1.f, pc.opacity};
        c.node = pc.node.index;
        dl.push(c);
        const ViewportOverlay& o = s->overlay;
        if (o.frameWidth > 0.f) {
            const Rect f = pc.content;
            const f32 t = o.frameWidth;
            const Color fc = pc.tint(o.frame);
            dl.rect({f.x, f.y, f.w, t}, fc);
            dl.rect({f.x, f.bottom() - t, f.w, t}, fc);
            dl.rect({f.x, f.y, t, f.h}, fc);
            dl.rect({f.right() - t, f.y, t, f.h}, fc);
        }
        if (!o.badge.empty()) {
            const f32 fs = pc.style.fontSize, bw = pc.ui.measureString(o.badge, fs) + 16.f, bh = fs + 8.f;
            const Rect b{pc.content.x + (pc.content.w - bw) * 0.5f, pc.content.y + 4.f + o.frameWidth, bw, bh};
            dl.rect(b, pc.tint(o.badgeColor), 3.f);
            pc.ui.paintString(dl, o.badge, b, pc.tint(o.badgeText), fs, TextAlignment::Center, pc.node.index);
        }
        if (o.lines.empty() && o.dots.empty()) return;
        const Rect r = c.rect;
        const f32 sx = r.w / f32(s->w), sy = r.h / f32(s->h);
        auto map = [&](Vec2 p) { return Vec2{r.x + p.x * sx, r.y + p.y * sy}; };
        dl.pushClip(r);
        for (const ViewportOverlay::Line& l : o.lines) dl.line(map(l.a), map(l.b), pc.tint(l.color), l.width);
        for (const ViewportOverlay::Dot& d : o.dots) {
            const Vec2 p = map(d.center);
            const f32 rad = d.radius;
            dl.roundedRect({p.x - rad, p.y - rad, rad * 2.f, rad * 2.f}, pc.tint(d.color), {rad, rad, rad, rad},
                           pc.tint(d.border), d.borderWidth);
        }
        dl.popClip();
    }
    static bool onEvent(EventContext& ec, const Event& e) {
        if (ec.phase != Phase::Target) return false;
        ViewportState* s = vpPool().get(ec.ui, ec.node);
        if (!s) return false;
        switch (e.type) {
        case EventType::PointerMove: case EventType::PointerDown: case EventType::PointerUp: case EventType::Wheel:
        case EventType::KeyDown: case EventType::KeyUp: break;
        default: return false;
        }
        const Rect r = letterbox(ec.ui.contentRect(ec.node), s->w, s->h);
        s->last = {e.type, {(e.pos.x - r.x) * f32(s->w) / std::max(1.f, r.w), (e.pos.y - r.y) * f32(s->h) / std::max(1.f, r.h)},
                   e.button, e.wheel, e.key};
        if (e.type == EventType::KeyDown || e.type == EventType::KeyUp) s->last.pos = {};
        ++s->count;
        ec.ui.emit(ec.node, SignalKind::Event, {.value = f32(u32(e.type))});
        return true;
    }
};

// =====================================================================================================================
// Code editor
struct Pos { u32 line = 0, col = 0; };
bool operator<(Pos a, Pos b) { return a.line != b.line ? a.line < b.line : a.col < b.col; }
bool operator==(Pos a, Pos b) { return a.line == b.line && a.col == b.col; }
struct CodeState {
    std::vector<std::string> lines{std::string()};
    Pos caret, anchor;
    u32 first = 0; // first visible line
    CodeLanguage lang = CodeLanguage::Lua;
    std::vector<std::pair<std::string, Pos>> undo, redo;
    bool dragging = false;
    std::string joined;
};
Pool<CodeState>& codePool() { static Pool<CodeState> p; return p; }
bool isKeyword(CodeLanguage l, std::string_view w) {
    static constexpr std::string_view lua[] = {"and", "break", "do", "else", "elseif", "end", "false", "for", "function", "goto", "if",
                                               "in", "local", "nil", "not", "or", "repeat", "return", "then", "true", "until", "while"};
    static constexpr std::string_view cpp[] = {"auto", "bool", "break", "case", "class", "const", "constexpr", "continue", "default",
                                               "do", "else", "enum", "false", "float", "for", "if", "int", "namespace", "nullptr",
                                               "return", "static", "struct", "switch", "template", "true", "using", "void", "while"};
    if (l == CodeLanguage::Lua) return std::find(std::begin(lua), std::end(lua), w) != std::end(lua);
    if (l == CodeLanguage::Cpp) return std::find(std::begin(cpp), std::end(cpp), w) != std::end(cpp);
    return false;
}
/// Token classes for one line (0 text, 1 keyword, 2 string, 3 number, 4 comment). Comments end at the line end.
void tokenize(CodeLanguage lang, std::string_view s, std::vector<u8>& out) {
    out.assign(s.size(), 0);
    if (lang == CodeLanguage::Plain) return;
    auto ident = [](char c) { return std::isalnum(u8(c)) || c == '_'; };
    for (std::size_t i = 0; i < s.size();) {
        const char c = s[i];
        const bool comment = (lang == CodeLanguage::Lua && s.substr(i, 2) == "--") ||
                             ((lang == CodeLanguage::Cpp || lang == CodeLanguage::Css) && (s.substr(i, 2) == "//" || s.substr(i, 2) == "/*"));
        if (comment) { std::fill(out.begin() + std::ptrdiff_t(i), out.end(), u8(4)); return; }
        if (c == '"' || c == '\'') {
            std::size_t j = i + 1;
            while (j < s.size() && s[j] != c) j += s[j] == '\\' ? 2 : 1;
            j = std::min(s.size(), j + 1);
            std::fill(out.begin() + std::ptrdiff_t(i), out.begin() + std::ptrdiff_t(j), u8(2));
            i = j;
        } else if (std::isdigit(u8(c)) || (c == '#' && lang == CodeLanguage::Css)) {
            std::size_t j = i + 1;
            while (j < s.size() && (std::isalnum(u8(s[j])) || s[j] == '.')) ++j;
            std::fill(out.begin() + std::ptrdiff_t(i), out.begin() + std::ptrdiff_t(j), u8(3));
            i = j;
        } else if (ident(c)) {
            std::size_t j = i;
            while (j < s.size() && ident(s[j])) ++j;
            if (isKeyword(lang, s.substr(i, j - i))) std::fill(out.begin() + std::ptrdiff_t(i), out.begin() + std::ptrdiff_t(j), u8(1));
            i = j;
        } else ++i;
    }
}
std::string joinLines(const CodeState& s) {
    std::string r;
    for (std::size_t i = 0; i < s.lines.size(); ++i) { if (i) r += '\n'; r += s.lines[i]; }
    return r;
}
void splitInto(CodeState& s, std::string_view t) {
    s.lines.clear();
    std::size_t at = 0;
    for (;;) {
        const std::size_t nl = t.find('\n', at);
        std::string line(t.substr(at, nl == std::string_view::npos ? std::string_view::npos : nl - at));
        if (!line.empty() && line.back() == '\r') line.pop_back();
        s.lines.push_back(std::move(line));
        if (nl == std::string_view::npos) break;
        at = nl + 1;
    }
}
u32 prevCp(std::string_view s, u32 c) { if (!c) return 0; --c; while (c && (u8(s[c]) & 0xC0) == 0x80) --c; return c; }
u32 nextCp(std::string_view s, u32 c) { if (c >= s.size()) return u32(s.size()); ++c; while (c < s.size() && (u8(s[c]) & 0xC0) == 0x80) ++c; return c; }

struct CodeEditW {
    static constexpr std::string_view kName = "codeedit";
    static constexpr A11yRole kRole = A11yRole::TextField;
    static constexpr bool kFocusable = true;
    static constexpr bool kInteractive = true;
    static Size measure(UiContext&, NodeHandle, Size) { return {420.f, 240.f}; }
    static f32 lineH(const PaintContext& pc) { return std::ceil(pc.style.fontSize * 1.35f); }
    static f32 lineH(UiContext& ui, NodeHandle n) { return std::ceil(ui.computed(n).fontSize * 1.35f); }
    static f32 gutter(UiContext& ui, NodeHandle n, const CodeState& s) {
        char b[16];
        std::snprintf(b, sizeof b, "%zu", s.lines.size());
        return ui.measureString(b, ui.computed(n).fontSize * 0.9f) + 18.f;
    }
    static u32 visible(UiContext& ui, NodeHandle n) { return std::max(1u, u32(ui.contentRect(n).h / lineH(ui, n))); }
    static void paint(const PaintContext& pc, DrawList& dl) {
        UiContext& ui = pc.ui;
        CodeState* s = codePool().get(ui, pc.node);
        if (!s) return;
        const Rect c = pc.content;
        const f32 lh = lineH(pc), fs = pc.style.fontSize, gw = gutter(ui, pc.node, *s);
        static const Color kTok[] = {Color::rgb(0xD4D4D4), Color::rgb(0x569CD6), Color::rgb(0xCE9178), Color::rgb(0xB5CEA8), Color::rgb(0x6A9955)};
        dl.pushClip(c);
        dl.rect(c, pc.tint(Color::rgb(0x1A1A20)));
        dl.rect({c.x, c.y, gw - 6.f, c.h}, pc.tint(Color::rgb(0x212128)));
        const Pos a = std::min(s->caret, s->anchor), b = std::max(s->caret, s->anchor);
        const bool focused = ui.focused() == pc.node;
        std::vector<u8> tok;
        const u32 vis = visible(ui, pc.node) + 1;
        for (u32 r = 0; r < vis && s->first + r < s->lines.size(); ++r) {
            const u32 li = s->first + r;
            const std::string& L = s->lines[li];
            const f32 y = c.y + f32(r) * lh, x0 = c.x + gw;
            if (li == s->caret.line && focused) dl.rect({x0 - 4.f, y, c.w - gw + 4.f, lh}, pc.tint(Color::rgb(0xFFFFFF, 0.04f)));
            char num[16];
            std::snprintf(num, sizeof num, "%u", li + 1);
            ui.paintString(dl, num, {c.x, y, gw - 12.f, lh}, pc.tint(li == s->caret.line ? Color::rgb(0xC8C8D0) : Color::rgb(0x6A6A76)),
                           fs * 0.9f, TextAlignment::End, pc.node.index);
            if (!(a == b) && li >= a.line && li <= b.line) { // selection
                const u32 sc = li == a.line ? a.col : 0, ecl = li == b.line ? b.col : u32(L.size());
                const f32 sx = x0 + ui.measureString(std::string_view(L).substr(0, sc), fs);
                const f32 ex = x0 + ui.measureString(std::string_view(L).substr(0, ecl), fs) + (li != b.line ? 6.f : 0.f);
                dl.rect({sx, y, std::max(0.f, ex - sx), lh}, pc.tint(Color::rgb(0x264F78)));
            }
            tokenize(s->lang, L, tok);
            for (std::size_t i = 0; i < L.size();) {
                std::size_t j = i;
                while (j < L.size() && tok[j] == tok[i]) ++j;
                const f32 x = x0 + ui.measureString(std::string_view(L).substr(0, i), fs);
                ui.paintString(dl, std::string_view(L).substr(i, j - i), {x, y, c.right() - x + 400.f, lh}, pc.tint(kTok[tok[i]]), fs,
                               TextAlignment::Start, pc.node.index);
                i = j;
            }
            if (focused && li == s->caret.line) {
                const f32 cx = x0 + ui.measureString(std::string_view(L).substr(0, s->caret.col), fs);
                dl.rect({cx, y + 2.f, 2.f, lh - 4.f}, pc.tint(Color::rgb(0xFFFFFF)));
                ui.setTextCaret(pc.node, {cx, y + 2.f, 2.f, lh - 4.f});
            }
        }
        dl.popClip();
    }
    static void snapshot(CodeState& s) {
        s.undo.push_back({joinLines(s), s.caret});
        if (s.undo.size() > 128) s.undo.erase(s.undo.begin());
        s.redo.clear();
    }
    static void eraseSel(CodeState& s) {
        if (s.caret == s.anchor) return;
        const Pos a = std::min(s.caret, s.anchor), b = std::max(s.caret, s.anchor);
        s.lines[a.line] = s.lines[a.line].substr(0, a.col) + s.lines[b.line].substr(b.col);
        s.lines.erase(s.lines.begin() + a.line + 1, s.lines.begin() + b.line + 1);
        s.caret = s.anchor = a;
    }
    static std::string selected(const CodeState& s) {
        const Pos a = std::min(s.caret, s.anchor), b = std::max(s.caret, s.anchor);
        if (a.line == b.line) return s.lines[a.line].substr(a.col, b.col - a.col);
        std::string r = s.lines[a.line].substr(a.col);
        for (u32 i = a.line + 1; i < b.line; ++i) { r += '\n'; r += s.lines[i]; }
        r += '\n';
        r += s.lines[b.line].substr(0, b.col);
        return r;
    }
    static void insert(CodeState& s, std::string_view t) {
        eraseSel(s);
        std::string tail = s.lines[s.caret.line].substr(s.caret.col);
        s.lines[s.caret.line].resize(s.caret.col);
        std::size_t at = 0;
        for (;;) {
            const std::size_t nl = t.find('\n', at);
            s.lines[s.caret.line] += t.substr(at, nl == std::string_view::npos ? std::string_view::npos : nl - at);
            if (nl == std::string_view::npos) break;
            s.lines.insert(s.lines.begin() + s.caret.line + 1, std::string());
            ++s.caret.line;
            at = nl + 1;
        }
        s.caret.col = u32(s.lines[s.caret.line].size());
        s.lines[s.caret.line] += tail;
        s.anchor = s.caret;
    }
    static void publish(UiContext& ui, NodeHandle n, CodeState& s, bool edited) {
        const u32 v = visible(ui, n);
        if (s.caret.line < s.first) s.first = s.caret.line;
        else if (s.caret.line >= s.first + v) s.first = s.caret.line - v + 1;
        if (edited) {
            s.joined = joinLines(s);
            ui.setText(n, s.joined);
            ui.emit(n, SignalKind::TextChanged, {.text = s.joined});
        }
        ui.invalidate(n);
    }
    static Pos hit(UiContext& ui, NodeHandle n, const CodeState& s, Vec2 p) {
        const Rect c = ui.contentRect(n);
        const f32 fs = ui.computed(n).fontSize;
        Pos r;
        r.line = std::min(u32(s.lines.size()) - 1, s.first + u32(std::max(0.f, (p.y - c.y) / lineH(ui, n))));
        const std::string& L = s.lines[r.line];
        const f32 x = p.x - c.x - gutter(ui, n, s);
        u32 col = 0;
        while (col < L.size()) {
            const u32 nx = nextCp(L, col);
            const f32 mid = (ui.measureString(std::string_view(L).substr(0, col), fs) + ui.measureString(std::string_view(L).substr(0, nx), fs)) * 0.5f;
            if (x < mid) break;
            col = nx;
        }
        r.col = col;
        return r;
    }
    static bool onEvent(EventContext& ec, const Event& e) {
        if (ec.phase != Phase::Target) return false;
        UiContext& ui = ec.ui;
        CodeState* sp = codePool().get(ui, ec.node);
        if (!sp) return false;
        CodeState& s = *sp;
        const bool shift = e.mods & kModShift, ctrl = e.mods & kModCtrl;
        switch (e.type) {
        case EventType::PointerDown:
            ui.setFocus(ec.node, FocusReason::Pointer);
            s.caret = hit(ui, ec.node, s, e.pos);
            if (!(e.mods & kModShift)) s.anchor = s.caret;
            s.dragging = true;
            publish(ui, ec.node, s, false);
            return true;
        case EventType::PointerMove:
            if (!s.dragging) return false;
            s.caret = hit(ui, ec.node, s, e.pos);
            publish(ui, ec.node, s, false);
            return true;
        case EventType::PointerUp: s.dragging = false; return true;
        case EventType::Wheel: {
            const i64 f = i64(s.first) - i64(std::lround(e.wheel.y * 3.f));
            s.first = u32(std::clamp<i64>(f, 0, i64(s.lines.size()) - 1));
            ui.invalidate(ec.node);
            return true;
        }
        case EventType::Text:
            if (e.text.empty() || ctrl) return false;
            snapshot(s);
            insert(s, e.text);
            publish(ui, ec.node, s, true);
            return true;
        case EventType::KeyDown: break;
        default: return false;
        }
        std::string& L = s.lines[s.caret.line];
        auto move = [&](Pos p) { s.caret = p; if (!shift) s.anchor = p; publish(ui, ec.node, s, false); return true; };
        switch (e.key) {
        case Key::Left:
            if (!shift && !(s.caret == s.anchor)) return move(std::min(s.caret, s.anchor));
            if (s.caret.col) return move({s.caret.line, prevCp(L, s.caret.col)});
            if (s.caret.line) return move({s.caret.line - 1, u32(s.lines[s.caret.line - 1].size())});
            return move(s.caret);
        case Key::Right:
            if (!shift && !(s.caret == s.anchor)) return move(std::max(s.caret, s.anchor));
            if (s.caret.col < L.size()) return move({s.caret.line, nextCp(L, s.caret.col)});
            if (s.caret.line + 1 < s.lines.size()) return move({s.caret.line + 1, 0});
            return move(s.caret);
        case Key::Up: case Key::Down: case Key::PageUp: case Key::PageDown: {
            const i64 d = e.key == Key::Up ? -1 : e.key == Key::Down ? 1 : (e.key == Key::PageUp ? -1 : 1) * i64(visible(ui, ec.node));
            const u32 ln = u32(std::clamp<i64>(i64(s.caret.line) + d, 0, i64(s.lines.size()) - 1));
            return move({ln, std::min(s.caret.col, u32(s.lines[ln].size()))});
        }
        case Key::Home: {
            if (ctrl) return move({0, 0});
            u32 ind = 0;
            while (ind < L.size() && (L[ind] == ' ' || L[ind] == '\t')) ++ind;
            return move({s.caret.line, s.caret.col == ind ? 0u : ind});
        }
        case Key::End:
            if (ctrl) return move({u32(s.lines.size()) - 1, u32(s.lines.back().size())});
            return move({s.caret.line, u32(L.size())});
        case Key::Backspace: case Key::Delete: {
            snapshot(s);
            if (s.caret == s.anchor) {
                if (e.key == Key::Backspace) {
                    if (s.caret.col) s.anchor = {s.caret.line, prevCp(L, s.caret.col)};
                    else if (s.caret.line) s.anchor = {s.caret.line - 1, u32(s.lines[s.caret.line - 1].size())};
                } else {
                    if (s.caret.col < L.size()) s.anchor = {s.caret.line, nextCp(L, s.caret.col)};
                    else if (s.caret.line + 1 < s.lines.size()) s.anchor = {s.caret.line + 1, 0};
                }
            }
            eraseSel(s);
            publish(ui, ec.node, s, true);
            return true;
        }
        case Key::Enter: {
            snapshot(s);
            std::string ind;
            for (char ch : L) { if (ch != ' ' && ch != '\t') break; ind += ch; }
            insert(s, "\n" + ind);
            publish(ui, ec.node, s, true);
            return true;
        }
        case Key::Tab:
            snapshot(s);
            insert(s, std::string(4 - s.caret.col % 4, ' '));
            publish(ui, ec.node, s, true);
            return true;
        case Key::A:
            if (!ctrl) return false;
            s.anchor = {0, 0};
            s.caret = {u32(s.lines.size()) - 1, u32(s.lines.back().size())};
            publish(ui, ec.node, s, false);
            return true;
        case Key::C: case Key::X:
            if (!ctrl) return false;
            if (!(s.caret == s.anchor)) ui.setClipboardText(selected(s));
            if (e.key == Key::X && !(s.caret == s.anchor)) { snapshot(s); eraseSel(s); publish(ui, ec.node, s, true); }
            return true;
        case Key::V: {
            if (!ctrl) return false;
            std::string clip;
            if (!ui.clipboardText(clip) || clip.empty()) return true;
            snapshot(s);
            insert(s, clip);
            publish(ui, ec.node, s, true);
            return true;
        }
        case Key::Z: case Key::Y: {
            if (!ctrl) return false;
            auto& from = (e.key == Key::Z && !shift) ? s.undo : s.redo;
            auto& to = (e.key == Key::Z && !shift) ? s.redo : s.undo;
            if (from.empty()) return true;
            to.push_back({joinLines(s), s.caret});
            splitInto(s, from.back().first);
            s.caret = s.anchor = from.back().second;
            from.pop_back();
            s.caret.line = std::min(s.caret.line, u32(s.lines.size()) - 1);
            s.caret.col = std::min(s.caret.col, u32(s.lines[s.caret.line].size()));
            s.anchor = s.caret;
            publish(ui, ec.node, s, true);
            return true;
        }
        default: return false;
        }
    }
};

// =====================================================================================================================
// Date-time edit
struct DtState { int f[5] = {1970, 1, 1, 0, 0}; u32 field = 0; std::string typed; NodeHandle popup; };
Pool<DtState>& dtPool() { static Pool<DtState> p; return p; }
int monthDays(int y, int m) {
    static const int d[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    return m == 2 && ((y % 4 == 0 && y % 100 != 0) || y % 400 == 0) ? 29 : d[(m - 1) % 12];
}
void dtNormalize(DtState& s) {
    s.f[0] = std::clamp(s.f[0], 1, 9999);
    s.f[1] = std::clamp(s.f[1], 1, 12);
    s.f[2] = std::clamp(s.f[2], 1, monthDays(s.f[0], s.f[1]));
    s.f[3] = std::clamp(s.f[3], 0, 23);
    s.f[4] = std::clamp(s.f[4], 0, 59);
}
i64 dtMinutes(const DtState& s) { return i64(daysFromCivil(s.f[0], s.f[1], s.f[2])) * 1440 + s.f[3] * 60 + s.f[4]; }
void dtSetFromMinutes(DtState& s, i64 m) {
    const i64 d = m >= 0 ? m / 1440 : -((-m + 1439) / 1440);
    const i64 r = m - d * 1440;
    civilFromDays(i32(d), s.f[0], s.f[1], s.f[2]);
    s.f[3] = int(r / 60);
    s.f[4] = int(r % 60);
}
void dtPublish(UiContext& ui, NodeHandle n, DtState& s) {
    dtNormalize(s);
    char b[32];
    std::snprintf(b, sizeof b, "%04d-%02d-%02d %02d:%02d", s.f[0], s.f[1], s.f[2], s.f[3], s.f[4]);
    ui.setText(n, b);
    ui.setValue(n, f32(daysFromCivil(s.f[0], s.f[1], s.f[2])), true); // days: exact in f32
    ui.invalidate(n);
}
void dtStep(DtState& s, int d) {
    switch (s.field) {
    case 0: s.f[0] += d; break;
    case 1: {
        int m = s.f[0] * 12 + (s.f[1] - 1) + d;
        s.f[0] = m / 12; s.f[1] = m % 12 + 1;
        break;
    }
    case 2: { int y, mo, dd; civilFromDays(daysFromCivil(s.f[0], s.f[1], s.f[2]) + d, y, mo, dd); s.f[0] = y; s.f[1] = mo; s.f[2] = dd; break; }
    case 3: dtSetFromMinutes(s, dtMinutes(s) + d * 60); return;
    default: dtSetFromMinutes(s, dtMinutes(s) + d); return;
    }
    s.f[2] = std::min(s.f[2], monthDays(s.f[0], s.f[1]));
}
struct DateTimeW {
    static constexpr std::string_view kName = "datetimeedit";
    static constexpr A11yRole kRole = A11yRole::SpinBox;
    static constexpr bool kFocusable = true;
    static constexpr bool kInteractive = true;
    static constexpr f32 kBtn = 22.f;
    static constexpr u32 kStart[5] = {0, 5, 8, 11, 14}, kLen[5] = {4, 2, 2, 2, 2};
    static Size measure(UiContext& ui, NodeHandle n, Size) {
        const f32 fs = ui.computed(n).fontSize;
        return {ui.measureString("0000-00-00 00:00", fs) + kBtn + 10.f, std::ceil(fs * 1.4f)};
    }
    static void paint(const PaintContext& pc, DrawList& dl) {
        UiContext& ui = pc.ui;
        DtState* s = dtPool().get(ui, pc.node);
        if (!s) return;
        const Rect c = pc.content;
        const std::string_view t = ui.text(pc.node);
        const f32 fs = pc.style.fontSize;
        const bool focused = ui.focused() == pc.node;
        if (focused && t.size() >= 16) {
            const f32 x0 = c.x + ui.measureString(t.substr(0, kStart[s->field]), fs);
            const f32 x1 = c.x + ui.measureString(t.substr(0, kStart[s->field] + kLen[s->field]), fs);
            dl.rect({x0 - 1.f, c.y + 1.f, x1 - x0 + 2.f, c.h - 2.f}, accentOf(pc).withAlpha(0.55f * pc.opacity), 3.f);
        }
        ui.paintString(dl, t, {c.x, c.y, c.w - kBtn, c.h}, pc.tint(pc.style.color), fs, TextAlignment::Start, pc.node.index);
        const Rect b{c.right() - kBtn, c.y, kBtn, c.h};
        dl.rect(b, pc.tint(Color::rgb(0xFFFFFF, 0.06f)), 3.f);
        // calendar glyph: a small page with a header bar
        const Rect g{b.x + 5.f, b.y + c.h * 0.5f - 6.f, 12.f, 12.f};
        dl.roundedRect(g, {}, {2.f, 2.f, 2.f, 2.f}, pc.tint(pc.style.color), 1.2f);
        dl.rect({g.x, g.y, g.w, 3.f}, pc.tint(pc.style.color), 1.f);
    }
    static void openPopup(UiContext& ui, NodeHandle n, DtState& s) {
        const Rect r = ui.rect(n);
        const NodeHandle card = detail::popupPanel(ui, n, {r.x, r.bottom() + 2.f});
        if (!ui.alive(card)) return;
        const NodeHandle cal = calendar(ui, card, s.f[0], s.f[1], s.f[2]);
        ui.setFocus(cal, FocusReason::Keyboard);
        auto pick = [n](UiContext& u, NodeHandle c, const SignalArgs&) {
            DtState* st = dtPool().get(u, n);
            if (!st) return;
            calendarDate(u, c, st->f[0], st->f[1], st->f[2]);
            dtPublish(u, n, *st);
        };
        ui.connect(cal, SignalKind::ValueChanged, pick);
        ui.connect(cal, SignalKind::Submitted, [n](UiContext& u, NodeHandle c, const SignalArgs&) { closePopup(u, c); (void)n; });
        s.popup = card;
    }
    static bool onEvent(EventContext& ec, const Event& e) {
        if (ec.phase != Phase::Target) return false;
        UiContext& ui = ec.ui;
        DtState* s = dtPool().get(ui, ec.node);
        if (!s) return false;
        const Rect c = ui.contentRect(ec.node);
        switch (e.type) {
        case EventType::PointerDown: {
            ui.setFocus(ec.node, FocusReason::Pointer);
            if (e.pos.x >= c.right() - kBtn) { openPopup(ui, ec.node, *s); return true; }
            const std::string_view t = ui.text(ec.node);
            const f32 fs = ui.computed(ec.node).fontSize;
            for (u32 k = 5; k-- > 0;)
                if (e.pos.x >= c.x + ui.measureString(t.substr(0, kStart[k]), fs) - 3.f) { s->field = k; break; }
            s->typed.clear();
            ui.invalidate(ec.node);
            return true;
        }
        case EventType::Wheel: dtStep(*s, e.wheel.y > 0 ? 1 : -1); dtPublish(ui, ec.node, *s); return true;
        case EventType::Text: {
            bool any = false;
            for (char ch : e.text) {
                if (ch < '0' || ch > '9') continue;
                any = true;
                s->typed += ch;
                int v = std::atoi(s->typed.c_str());
                if (s->field == 0 && s->typed.size() < 4) continue;
                s->f[s->field] = v;
                if (s->typed.size() >= kLen[s->field]) { s->typed.clear(); s->field = std::min(4u, s->field + 1); }
            }
            if (any) dtPublish(ui, ec.node, *s);
            return any;
        }
        case EventType::KeyDown:
            switch (e.key) {
            case Key::Left: s->field = s->field ? s->field - 1 : 0; s->typed.clear(); ui.invalidate(ec.node); return true;
            case Key::Right: s->field = std::min(4u, s->field + 1); s->typed.clear(); ui.invalidate(ec.node); return true;
            case Key::Up: dtStep(*s, 1); dtPublish(ui, ec.node, *s); return true;
            case Key::Down: dtStep(*s, -1); dtPublish(ui, ec.node, *s); return true;
            case Key::Space: openPopup(ui, ec.node, *s); return true;
            default: return false;
            }
        default: return false;
        }
    }
};

// =====================================================================================================================
// Video (stub decoder)
struct VideoState {
    const VideoSource* src = nullptr;
    Image img;
    u32 index = 0, frame = 0, decoded = 0, decodedFrame = 0xFFFFFFFFu;
    bool playing = false;
    f32 clock = 0.f;
    TimerHandle timer{};
    bool scrubbing = false;
};
Pool<VideoState>& videoPool() { static Pool<VideoState> p; return p; }
void decode(VideoState& s) {
    if (!s.src || !s.src->frame || s.decodedFrame == s.frame) return;
    s.src->frame(s.src->user, s.frame, s.img.pixels.data());
    s.decodedFrame = s.frame;
    ++s.decoded;
}
void patternFrame(void*, u32 index, u32* px) {
    constexpr u32 W = 160, H = 90;
    for (u32 y = 0; y < H; ++y)
        for (u32 x = 0; x < W; ++x) {
            const u32 bar = x * 7 / W; // SMPTE-ish colour bars
            static const u32 bars[7] = {0xC0C0C0, 0x00C0C0, 0xC0C000, 0x00C000, 0xC000C0, 0x0000C0, 0xC00000}; // 0xBBGGRR
            u32 c = bars[bar];
            const u32 bx = (index * 2) % (W + 20);
            if (y > 60 && x + 20 >= bx && x < bx) c = 0xFFFFFF; // moving block
            if (y > 60 && y < 64) c = ((x / 8 + index) & 1) ? 0x202020 : 0xE0E0E0;
            px[y * W + x] = 0xFF000000u | c;
        }
}
struct VideoW {
    static constexpr std::string_view kName = "video";
    static constexpr A11yRole kRole = A11yRole::Image;
    static constexpr bool kFocusable = true;
    static constexpr bool kInteractive = true;
    static constexpr f32 kBar = 26.f;
    static Size measure(UiContext& ui, NodeHandle n, Size) {
        const VideoState* s = videoPool().get(ui, n);
        const f32 w = 320.f;
        return {w, (s && s->src && s->src->width ? w * f32(s->src->height) / f32(s->src->width) : 180.f) + kBar};
    }
    static void paint(const PaintContext& pc, DrawList& dl) {
        UiContext& ui = pc.ui;
        VideoState* s = videoPool().get(ui, pc.node);
        if (!s || !s->src) return;
        decode(*s);
        const Rect c = pc.content;
        const Rect pic{c.x, c.y, c.w, c.h - kBar};
        dl.rect(pic, pc.tint(Color::rgb(0x000000)));
        DrawCmd im;
        im.op = DrawOp::Image;
        im.image = s->index;
        im.rect = letterbox(pic, s->src->width, s->src->height);
        im.fill = Color{1.f, 1.f, 1.f, pc.opacity};
        im.node = pc.node.index;
        dl.push(im);
        const Rect bar{c.x, c.bottom() - kBar, c.w, kBar};
        dl.rect(bar, pc.tint(Color::rgb(0x202027)));
        // play / pause glyph
        const f32 cy = bar.y + kBar * 0.5f;
        if (s->playing) {
            dl.rect({bar.x + 9.f, cy - 6.f, 4.f, 12.f}, pc.tint(pc.style.color));
            dl.rect({bar.x + 16.f, cy - 6.f, 4.f, 12.f}, pc.tint(pc.style.color));
        } else
            for (int i = 0; i < 12; ++i) dl.rect({bar.x + 9.f + f32(i), cy - 6.f + f32(i) * 0.5f, 1.f, 12.f - f32(i)}, pc.tint(pc.style.color));
        const Rect track{bar.x + 30.f, cy - 2.f, bar.w - 100.f, 4.f};
        const f32 f = s->src->frames > 1 ? f32(s->frame) / f32(s->src->frames - 1) : 0.f;
        dl.rect(track, pc.tint(Color::rgb(0x3A3A45)), 2.f);
        dl.rect({track.x, track.y, track.w * f, 4.f}, accentOf(pc), 2.f);
        dot(dl, {track.x + track.w * f, cy}, 6.f, pc.tint(Color::rgb(0xFFFFFF)));
        char b[32];
        const f32 secs = f32(s->frame) / std::max(1.f, s->src->fps);
        std::snprintf(b, sizeof b, "%d:%02d.%02d", int(secs) / 60, int(secs) % 60, int(std::fmod(secs, 1.f) * 100.f));
        ui.paintString(dl, b, {track.right() + 8.f, bar.y, 62.f, kBar}, pc.tint(pc.style.color), pc.style.fontSize * 0.8f,
                       TextAlignment::Start, pc.node.index);
    }
    static void seekX(UiContext& ui, NodeHandle n, VideoState& s, f32 x) {
        const Rect c = ui.contentRect(n);
        const f32 tx = c.x + 30.f, tw = c.w - 100.f;
        videoSeek(ui, n, u32(std::lround(std::clamp((x - tx) / tw, 0.f, 1.f) * f32(s.src->frames ? s.src->frames - 1 : 0))));
    }
    static bool onEvent(EventContext& ec, const Event& e) {
        if (ec.phase != Phase::Target) return false;
        UiContext& ui = ec.ui;
        VideoState* s = videoPool().get(ui, ec.node);
        if (!s || !s->src) return false;
        const Rect c = ui.contentRect(ec.node);
        switch (e.type) {
        case EventType::PointerDown:
            ui.setFocus(ec.node, FocusReason::Pointer);
            if (e.pos.y >= c.bottom() - kBar && e.pos.x < c.x + 28.f) { videoPlay(ui, ec.node, !s->playing); return true; }
            if (e.pos.y >= c.bottom() - kBar) { s->scrubbing = true; seekX(ui, ec.node, *s, e.pos.x); return true; }
            videoPlay(ui, ec.node, !s->playing);
            return true;
        case EventType::PointerMove: if (!s->scrubbing) return false; seekX(ui, ec.node, *s, e.pos.x); return true;
        case EventType::PointerUp: s->scrubbing = false; return true;
        case EventType::KeyDown:
            if (e.key == Key::Space) { videoPlay(ui, ec.node, !s->playing); return true; }
            if (e.key == Key::Right) { videoSeek(ui, ec.node, s->frame + 1); return true; }
            if (e.key == Key::Left) { videoSeek(ui, ec.node, s->frame ? s->frame - 1 : 0); return true; }
            return false;
        default: return false;
        }
    }
};

} // namespace

void registerTierD2(UiContext& ui) {
    if (ui.registry().find("codeedit") != 0xFFFFFFFFu) return;
    ui.registry().registerWidget<NodeGraphW>();
    ui.registry().registerWidget<CurveW>();
    ui.registry().registerWidget<TimelineW>();
    ui.registry().registerWidget<ViewportW>();
    ui.registry().registerWidget<CodeEditW>();
    ui.registry().registerWidget<DateTimeW>();
    ui.registry().registerWidget<VideoW>();
}

// ---- font picker ------------------------------------------------------------------------------------------------------
NodeHandle fontPicker(UiContext& ui, NodeHandle parent, std::string_view families, f32 size, std::string_view id) {
    registerTierD2(ui);
    const NodeHandle p = panel(ui, parent, id);
    ui.addClass(p, "fontpicker");
    ui.setA11y(p, A11yRole::Group, "Font");
    FontState& s = fontPool().create(ui, p);
    s.list = listView(ui, p, families.empty() ? std::string_view("Open Sans\nSans Serif\nSerif\nMonospace\nSystem UI") : families, 0);
    ui.addClass(s.list, "fontlist");
    const NodeHandle row = hbox(ui, p);
    label(ui, row, "Size");
    s.size = spinBox(ui, row, 6.f, 96.f, size, 1.f);
    s.preview = label(ui, p, "");
    ui.addClass(s.preview, "fontpreview");
    ui.connect(s.list, SignalKind::ValueChanged, [p](UiContext& u, NodeHandle, const SignalArgs&) { fontRefresh(u, p, true); });
    ui.connect(s.size, SignalKind::ValueChanged, [p](UiContext& u, NodeHandle, const SignalArgs&) { fontRefresh(u, p, true); });
    fontRefresh(ui, p, false);
    return p;
}
std::string fontPickerFamily(const UiContext& ui, NodeHandle picker) {
    const FontState* s = fontPool().get(ui, picker);
    return s ? s->family : std::string();
}
f32 fontPickerSize(const UiContext& ui, NodeHandle picker) {
    const FontState* s = fontPool().get(ui, picker);
    return s ? ui.value(s->size) : 0.f;
}

// ---- file dialog ------------------------------------------------------------------------------------------------------
NodeHandle fileDialog(UiContext& ui, std::string_view title, std::string_view directory, std::string_view filter, bool save,
                      std::string_view id) {
    registerTierD2(ui);
    const NodeHandle dlg = dialog(ui, title, id);
    if (!ui.alive(dlg)) return dlg;
    ui.addClass(dlg, "filedialog");
    FileDlgState& s = filePool().create(ui, dlg);
    s.save = save;
    for (std::size_t at = 0; at <= filter.size();) {
        const std::size_t e = std::min(filter.find(';', at), filter.size());
        if (e > at) s.filters.emplace_back(filter.substr(at, e - at));
        at = e + 1;
    }
    const NodeHandle body = dialogBody(ui, dlg);
    const NodeHandle top = hbox(ui, body);
    const NodeHandle up = toolButton(ui, top, "Up");
    setTooltip(ui, up, "Parent folder");
    s.path = lineEdit(ui, top, "");
    ui.addClass(s.path, "grow");
    s.list = listView(ui, body, "", -1);
    ui.addClass(s.list, "filelist");
    const NodeHandle nameRow = hbox(ui, body);
    label(ui, nameRow, save ? "Save as" : "File");
    s.name = lineEdit(ui, nameRow, "");
    ui.addClass(s.name, "grow");
    const NodeHandle buttons = hbox(ui, body);
    ui.addClass(buttons, "buttons");
    const NodeHandle ok = button(ui, buttons, save ? "Save" : "Open");
    const NodeHandle cancel = button(ui, buttons, "Cancel");
    ui.connect(up, SignalKind::Clicked, [dlg](UiContext& u, NodeHandle, const SignalArgs&) {
        FileDlgState* st = filePool().get(u, dlg);
        if (st) fileDialogNavigate(u, dlg, std::filesystem::path(st->dir).parent_path().generic_string());
    });
    ui.connect(s.path, SignalKind::Submitted, [dlg](UiContext& u, NodeHandle n, const SignalArgs&) {
        fileDialogNavigate(u, dlg, std::string(u.text(n)));
    });
    ui.connect(s.list, SignalKind::TextChanged, [dlg](UiContext& u, NodeHandle, const SignalArgs& a) {
        FileDlgState* st = filePool().get(u, dlg);
        if (st && !a.text.empty() && a.text.back() != '/') u.setText(st->name, a.text);
    });
    ui.connect(s.list, SignalKind::Submitted, [dlg](UiContext& u, NodeHandle n, const SignalArgs&) {
        if (u.value(n) >= 0.f) fileDialogActivate(u, dlg, u32(u.value(n)));
    });
    auto accept = [dlg](UiContext& u, NodeHandle, const SignalArgs&) {
        FileDlgState* st = filePool().get(u, dlg);
        if (st) fileAccept(u, dlg, *st, std::string(u.text(st->name)));
    };
    ui.connect(ok, SignalKind::Clicked, accept);
    ui.connect(s.name, SignalKind::Submitted, accept);
    ui.connect(cancel, SignalKind::Clicked, [dlg](UiContext& u, NodeHandle, const SignalArgs&) {
        closeDialog(u, dlg);
        u.emit(dlg, SignalKind::Submitted, {.text = {}});
    });
    fileDialogNavigate(ui, dlg, directory);
    return dlg;
}
bool fileDialogNavigate(UiContext& ui, NodeHandle dlg, std::string_view directory) {
    FileDlgState* s = filePool().get(ui, dlg);
    if (!s) return false;
    namespace fs = std::filesystem;
    std::error_code ec;
    const fs::path dir = fs::absolute(fs::path(std::string(directory)), ec).lexically_normal();
    if (ec || !fs::is_directory(dir, ec)) return false;
    std::vector<std::string> folders, files;
    for (fs::directory_iterator it(dir, fs::directory_options::skip_permission_denied, ec), end; !ec && it != end; it.increment(ec)) {
        const std::string name = it->path().filename().string();
        if (name.empty() || name[0] == '.') continue;
        std::error_code e2;
        if (it->is_directory(e2)) folders.push_back(name + "/");
        else if (s->filters.empty() || std::any_of(s->filters.begin(), s->filters.end(), [&](const std::string& f) { return globMatch(f, name); }))
            files.push_back(name);
    }
    auto ci = [](const std::string& a, const std::string& b) {
        return std::lexicographical_compare(a.begin(), a.end(), b.begin(), b.end(), [](char x, char y) { return std::tolower(u8(x)) < std::tolower(u8(y)); });
    };
    std::sort(folders.begin(), folders.end(), ci);
    std::sort(files.begin(), files.end(), ci);
    s->dir = dir.generic_string();
    if (s->dir.size() > 1 && s->dir.back() == '/') s->dir.pop_back();
    s->entries = std::move(folders);
    s->entries.insert(s->entries.end(), files.begin(), files.end());
    std::string items;
    for (const std::string& e : s->entries) { if (!items.empty()) items += '\n'; items += e; }
    ui.setText(s->list, items);
    ui.setValue(s->list, -1.f);
    ui.setText(s->path, s->dir);
    ui.editBuffer(s->path).setText(s->dir);
    ui.editBuffer(s->path).end(false);
    return true;
}
std::string fileDialogDirectory(const UiContext& ui, NodeHandle dlg) {
    const FileDlgState* s = filePool().get(ui, dlg);
    return s ? s->dir : std::string();
}
u32 fileDialogEntryCount(const UiContext& ui, NodeHandle dlg) {
    const FileDlgState* s = filePool().get(ui, dlg);
    return s ? u32(s->entries.size()) : 0u;
}
std::string fileDialogEntry(const UiContext& ui, NodeHandle dlg, u32 i) {
    const FileDlgState* s = filePool().get(ui, dlg);
    return s && i < s->entries.size() ? s->entries[i] : std::string();
}
std::string fileDialogActivate(UiContext& ui, NodeHandle dlg, u32 i) {
    FileDlgState* s = filePool().get(ui, dlg);
    if (!s || i >= s->entries.size()) return {};
    const std::string e = s->entries[i];
    if (e.back() == '/') { fileDialogNavigate(ui, dlg, joinPath(s->dir, e.substr(0, e.size() - 1))); return {}; }
    s->accepted.clear();
    fileAccept(ui, dlg, *s, e);
    return s->accepted;
}

// ---- node graph -------------------------------------------------------------------------------------------------------
NodeHandle nodeGraph(UiContext& ui, NodeHandle parent, std::string_view id) {
    registerTierD2(ui);
    const NodeHandle n = make(ui, "nodegraph", parent, id);
    if (!ui.alive(n)) return n;
    graphPool().create(ui, n);
    ui.setRange(n, -1.f, 1e9f, 1.f);
    ui.setValue(n, -1.f);
    ui.setA11y(n, A11yRole::Group, "Node graph");
    return n;
}
u32 nodeGraphAddNode(UiContext& ui, NodeHandle g, std::string_view title, Vec2 pos, std::span<const GraphPin> in, std::span<const GraphPin> out) {
    GraphState* s = graphPool().get(ui, g);
    if (!s) return 0xFFFFFFFFu;
    GNode nd;
    nd.title = title;
    nd.pos = pos;
    for (const GraphPin& p : in) nd.in.push_back({std::string(p.name), p.type});
    for (const GraphPin& p : out) nd.out.push_back({std::string(p.name), p.type});
    s->nodes.push_back(std::move(nd));
    ui.invalidate(g);
    return u32(s->nodes.size() - 1);
}
bool nodeGraphConnect(UiContext& ui, NodeHandle g, u32 a, u32 op, u32 b, u32 ip) {
    GraphState* s = graphPool().get(ui, g);
    if (!s || !gconnect(*s, a, op, b, ip)) return false;
    ui.invalidate(g);
    ui.emit(g, SignalKind::ValueChanged, {.value = f32(s->links.size())});
    return true;
}
bool nodeGraphDisconnect(UiContext& ui, NodeHandle g, u32 b, u32 ip) {
    GraphState* s = graphPool().get(ui, g);
    if (!s) return false;
    const std::size_t before = s->links.size();
    std::erase_if(s->links, [&](const GLink& l) { return l.to == b && l.inPin == ip; });
    if (before == s->links.size()) return false;
    ui.invalidate(g);
    ui.emit(g, SignalKind::ValueChanged, {.value = f32(s->links.size())});
    return true;
}
bool nodeGraphRemoveNode(UiContext& ui, NodeHandle g, u32 k) {
    GraphState* s = graphPool().get(ui, g);
    if (!s || k >= s->nodes.size() || !s->nodes[k].alive) return false;
    s->nodes[k].alive = false;
    std::erase_if(s->links, [&](const GLink& l) { return l.to == k || l.from == k; });
    if (ui.value(g) == f32(k)) ui.setValue(g, -1.f);
    ui.invalidate(g);
    ui.emit(g, SignalKind::ValueChanged, {.value = f32(s->links.size())});
    return true;
}
u32 nodeGraphNodeCount(const UiContext& ui, NodeHandle g) {
    const GraphState* s = graphPool().get(ui, g);
    return s ? u32(std::count_if(s->nodes.begin(), s->nodes.end(), [](const GNode& n) { return n.alive; })) : 0u;
}
u32 nodeGraphLinkCount(const UiContext& ui, NodeHandle g) {
    const GraphState* s = graphPool().get(ui, g);
    return s ? u32(s->links.size()) : 0u;
}
Vec2 nodeGraphNodePos(const UiContext& ui, NodeHandle g, u32 k) {
    const GraphState* s = graphPool().get(ui, g);
    return s && k < s->nodes.size() ? s->nodes[k].pos : Vec2{};
}
Vec2 nodeGraphPinPos(const UiContext& ui, NodeHandle g, u32 k, u32 pin, bool output) {
    const GraphState* s = graphPool().get(ui, g);
    return s && k < s->nodes.size() ? gpin(ui, g, *s, k, pin, output) : Vec2{};
}
std::vector<u32> nodeGraphTopoOrder(const UiContext& ui, NodeHandle g) {
    std::vector<u32> order;
    const GraphState* s = graphPool().get(ui, g);
    if (!s) return order;
    std::vector<u32> indeg(s->nodes.size(), 0);
    for (const GLink& l : s->links) ++indeg[l.to];
    std::vector<u32> ready;
    for (u32 i = 0; i < s->nodes.size(); ++i) if (s->nodes[i].alive && !indeg[i]) ready.push_back(i);
    while (!ready.empty()) {
        const u32 k = ready.front();
        ready.erase(ready.begin());
        order.push_back(k);
        for (const GLink& l : s->links) if (l.from == k && --indeg[l.to] == 0) ready.push_back(l.to);
    }
    if (order.size() != u32(nodeGraphNodeCount(ui, g))) order.clear();
    return order;
}

// ---- curve editor -----------------------------------------------------------------------------------------------------
NodeHandle curveEditor(UiContext& ui, NodeHandle parent, f32 t0, f32 t1, f32 v0, f32 v1, std::string_view id) {
    registerTierD2(ui);
    const NodeHandle n = make(ui, "curveeditor", parent, id);
    if (!ui.alive(n)) return n;
    CurveState& s = curvePool().create(ui, n);
    s.t0 = t0; s.t1 = t1 > t0 ? t1 : t0 + 1.f; s.v0 = v0; s.v1 = v1 > v0 ? v1 : v0 + 1.f;
    ui.setRange(n, -1.f, 1e9f, 1.f);
    ui.setValue(n, -1.f);
    ui.setA11y(n, A11yRole::Group, "Curve");
    return n;
}
u32 curveAddKey(UiContext& ui, NodeHandle c, f32 t, f32 v, CurveInterp interp) {
    CurveState* s = curvePool().get(ui, c);
    if (!s) return 0;
    const auto it = std::lower_bound(s->keys.begin(), s->keys.end(), t, [](const CKey& k, f32 x) { return k.t < x; });
    const u32 i = u32(it - s->keys.begin());
    s->keys.insert(it, {t, v, interp});
    CurveW::changed(ui, c, *s);
    return i;
}
u32 curveKeyCount(const UiContext& ui, NodeHandle c) { const CurveState* s = curvePool().get(ui, c); return s ? u32(s->keys.size()) : 0u; }
Vec2 curveKey(const UiContext& ui, NodeHandle c, u32 i) {
    const CurveState* s = curvePool().get(ui, c);
    return s && i < s->keys.size() ? Vec2{s->keys[i].t, s->keys[i].v} : Vec2{};
}
f32 curveEvaluate(const UiContext& ui, NodeHandle c, f32 t) { const CurveState* s = curvePool().get(ui, c); return s ? curveEval(*s, t) : 0.f; }

// ---- timeline ---------------------------------------------------------------------------------------------------------
NodeHandle timeline(UiContext& ui, NodeHandle parent, f32 duration, std::string_view id) {
    registerTierD2(ui);
    const NodeHandle n = make(ui, "timeline", parent, id);
    if (!ui.alive(n)) return n;
    tlPool().create(ui, n).duration = std::max(0.1f, duration);
    ui.setRange(n, 0.f, std::max(0.1f, duration), 0.f);
    ui.setValue(n, 0.f);
    ui.setA11y(n, A11yRole::Slider, "Timeline");
    return n;
}
u32 timelineAddTrack(UiContext& ui, NodeHandle tl, std::string_view name) {
    TimelineState* s = tlPool().get(ui, tl);
    if (!s) return 0;
    s->tracks.push_back({std::string(name), {}});
    ui.setText(tl, ""); // re-measure (height follows the track count)
    ui.markAllDirty();
    return u32(s->tracks.size() - 1);
}
u32 timelineAddClip(UiContext& ui, NodeHandle tl, u32 track, f32 start, f32 end, std::string_view label, Color color) {
    TimelineState* s = tlPool().get(ui, tl);
    if (!s || track >= s->tracks.size()) return 0;
    s->tracks[track].clips.push_back({start, std::max(start, end), std::string(label), color});
    ui.invalidate(tl);
    return u32(s->tracks[track].clips.size() - 1);
}
void timelineSetView(UiContext& ui, NodeHandle tl, f32 startSec, f32 spanSec) {
    TimelineState* s = tlPool().get(ui, tl);
    if (!s) return;
    if (spanSec <= 0.f) s->viewSpan = 0.f;
    else TimelineW::setView(*s, startSec, spanSec);
    ui.invalidate(tl);
}
Vec2 timelineView(const UiContext& ui, NodeHandle tl) {
    const TimelineState* s = tlPool().get(ui, tl);
    return s ? Vec2{s->viewStart, s->viewSpan} : Vec2{};
}
void timelineSetSnap(UiContext& ui, NodeHandle tl, f32 sec) { if (TimelineState* s = tlPool().get(ui, tl)) s->snap = std::max(0.f, sec); }
Vec2 timelineClip(const UiContext& ui, NodeHandle tl, u32 track, u32 clip) {
    const TimelineState* s = tlPool().get(ui, tl);
    if (!s || track >= s->tracks.size() || clip >= s->tracks[track].clips.size()) return {};
    return {s->tracks[track].clips[clip].start, s->tracks[track].clips[clip].end};
}
u32 timelineActiveClips(const UiContext& ui, NodeHandle tl, f32 t) {
    const TimelineState* s = tlPool().get(ui, tl);
    u32 n = 0;
    if (s) for (const Track& tr : s->tracks) for (const Clip& c : tr.clips) n += t >= c.start && t < c.end;
    return n;
}

// ---- viewport ---------------------------------------------------------------------------------------------------------
NodeHandle viewportWidget(UiContext& ui, NodeHandle parent, u32 imageIndex, u32 w, u32 h, std::string_view id) {
    registerTierD2(ui);
    const NodeHandle n = make(ui, "viewport", parent, id);
    if (!ui.alive(n)) return n;
    vpPool().create(ui, n);
    viewportSetImage(ui, n, imageIndex, w, h);
    ui.setA11y(n, A11yRole::Image, "Viewport");
    return n;
}
void viewportSetImage(UiContext& ui, NodeHandle vp, u32 imageIndex, u32 w, u32 h) {
    ViewportState* s = vpPool().get(ui, vp);
    if (!s) return;
    s->image = imageIndex; s->w = std::max(1u, w); s->h = std::max(1u, h);
    ui.invalidate(vp);
}
Rect viewportImageRect(const UiContext& ui, NodeHandle vp) {
    const ViewportState* s = vpPool().get(ui, vp);
    return s ? letterbox(ui.contentRect(vp), s->w, s->h) : Rect{};
}
const ViewportInput& viewportLastInput(const UiContext& ui, NodeHandle vp) {
    static const ViewportInput none;
    const ViewportState* s = vpPool().get(ui, vp);
    return s ? s->last : none;
}
void viewportSetOverlay(UiContext& ui, NodeHandle vp, ViewportOverlay overlay) {
    ViewportState* s = vpPool().get(ui, vp);
    if (!s) return;
    s->overlay = std::move(overlay);
    ui.invalidate(vp);
}
const ViewportOverlay& viewportOverlay(const UiContext& ui, NodeHandle vp) {
    static const ViewportOverlay none;
    const ViewportState* s = vpPool().get(ui, vp);
    return s ? s->overlay : none;
}
u32 viewportInputCount(const UiContext& ui, NodeHandle vp) { const ViewportState* s = vpPool().get(ui, vp); return s ? s->count : 0u; }

// ---- code editor ------------------------------------------------------------------------------------------------------
NodeHandle codeEdit(UiContext& ui, NodeHandle parent, std::string_view text, CodeLanguage lang, std::string_view id) {
    registerTierD2(ui);
    const NodeHandle n = make(ui, "codeedit", parent, id);
    if (!ui.alive(n)) return n;
    codePool().create(ui, n).lang = lang;
    ui.setA11y(n, A11yRole::TextField, "Code");
    codeEditSetText(ui, n, text);
    return n;
}
void codeEditSetText(UiContext& ui, NodeHandle ed, std::string_view text) {
    CodeState* s = codePool().get(ui, ed);
    if (!s) return;
    splitInto(*s, text);
    s->caret = s->anchor = {};
    s->first = 0;
    s->undo.clear();
    s->redo.clear();
    s->joined = joinLines(*s);
    ui.setText(ed, s->joined);
    ui.invalidate(ed);
}
std::string codeEditText(const UiContext& ui, NodeHandle ed) { const CodeState* s = codePool().get(ui, ed); return s ? joinLines(*s) : std::string(); }
u32 codeEditLineCount(const UiContext& ui, NodeHandle ed) { const CodeState* s = codePool().get(ui, ed); return s ? u32(s->lines.size()) : 0u; }
void codeEditCaret(const UiContext& ui, NodeHandle ed, u32& line, u32& col) {
    const CodeState* s = codePool().get(ui, ed);
    line = s ? s->caret.line : 0;
    col = s ? s->caret.col : 0;
}
void codeEditSetCaret(UiContext& ui, NodeHandle ed, u32 line, u32 col) {
    CodeState* s = codePool().get(ui, ed);
    if (!s) return;
    s->caret.line = std::min(line, u32(s->lines.size()) - 1);
    s->caret.col = std::min(col, u32(s->lines[s->caret.line].size()));
    s->anchor = s->caret;
    CodeEditW::publish(ui, ed, *s, false);
}
u8 codeEditTokenAt(const UiContext& ui, NodeHandle ed, u32 line, u32 col) {
    const CodeState* s = codePool().get(ui, ed);
    if (!s || line >= s->lines.size() || col >= s->lines[line].size()) return 0;
    std::vector<u8> t;
    tokenize(s->lang, s->lines[line], t);
    return t[col];
}

// ---- date-time edit ---------------------------------------------------------------------------------------------------
NodeHandle dateTimeEdit(UiContext& ui, NodeHandle parent, int y, int m, int d, int hh, int mm, std::string_view id) {
    registerTierD2(ui);
    const NodeHandle n = make(ui, "datetimeedit", parent, id);
    if (!ui.alive(n)) return n;
    dtPool().create(ui, n);
    ui.setA11y(n, A11yRole::SpinBox, "Date and time");
    ui.setRange(n, -1.0e7f, 1.0e7f);
    setDateTimeEditValue(ui, n, y, m, d, hh, mm);
    return n;
}
void dateTimeEditValue(const UiContext& ui, NodeHandle ed, int& y, int& m, int& d, int& hh, int& mm) {
    const DtState* s = dtPool().get(ui, ed);
    const DtState z;
    const DtState& r = s ? *s : z;
    y = r.f[0]; m = r.f[1]; d = r.f[2]; hh = r.f[3]; mm = r.f[4];
}
void setDateTimeEditValue(UiContext& ui, NodeHandle ed, int y, int m, int d, int hh, int mm) {
    DtState* s = dtPool().get(ui, ed);
    if (!s) return;
    s->f[0] = y; s->f[1] = m; s->f[2] = d; s->f[3] = hh; s->f[4] = mm;
    dtPublish(ui, ed, *s);
}
i64 dateTimeEditMinutes(const UiContext& ui, NodeHandle ed) { const DtState* s = dtPool().get(ui, ed); return s ? dtMinutes(*s) : 0; }
u32 dateTimeEditField(const UiContext& ui, NodeHandle ed) { const DtState* s = dtPool().get(ui, ed); return s ? s->field : 0u; }

// ---- video ------------------------------------------------------------------------------------------------------------
const VideoSource& testPatternSource() {
    static const VideoSource src{nullptr, 160, 90, 300, 30.f, &patternFrame};
    return src;
}
NodeHandle videoWidget(UiContext& ui, NodeHandle parent, const VideoSource* source, std::string_view id) {
    registerTierD2(ui);
    const NodeHandle n = make(ui, "video", parent, id);
    if (!ui.alive(n)) return n;
    VideoState& s = videoPool().create(ui, n);
    s.src = source;
    if (source) s.img.resize(int(source->width), int(source->height), 0xFF000000u);
    ui.setRange(n, 0.f, source && source->frames ? f32(source->frames - 1) : 0.f, 1.f);
    ui.setValue(n, 0.f);
    ui.setA11y(n, A11yRole::Image, "Video");
    decode(s);
    return n;
}
const Image* videoImage(const UiContext& ui, NodeHandle v) { const VideoState* s = videoPool().get(ui, v); return s ? &s->img : nullptr; }
void videoSetImageIndex(UiContext& ui, NodeHandle v, u32 index) { if (VideoState* s = videoPool().get(ui, v)) { s->index = index; ui.invalidate(v); } }
void videoPlay(UiContext& ui, NodeHandle v, bool play) {
    VideoState* s = videoPool().get(ui, v);
    if (!s || !s->src || s->playing == play) return;
    s->playing = play;
    if (play) {
        s->timer = ui.startTimer(1.f / std::max(1.f, s->src->fps), true, [v](UiContext& u) {
            VideoState* st = videoPool().get(u, v);
            if (!st || !st->playing) return;
            const u32 next = st->frame + 1 >= st->src->frames ? 0 : st->frame + 1;
            videoSeek(u, v, next);
        });
    } else ui.cancelTimer(s->timer);
    ui.emit(v, SignalKind::Toggled, {.flag = play});
    ui.invalidate(v);
}
bool videoPlaying(const UiContext& ui, NodeHandle v) { const VideoState* s = videoPool().get(ui, v); return s && s->playing; }
void videoSeek(UiContext& ui, NodeHandle v, u32 frame) {
    VideoState* s = videoPool().get(ui, v);
    if (!s || !s->src) return;
    s->frame = std::min(frame, s->src->frames ? s->src->frames - 1 : 0);
    decode(*s);
    ui.setValue(v, f32(s->frame), true);
    ui.invalidate(v);
}
u32 videoFrame(const UiContext& ui, NodeHandle v) { const VideoState* s = videoPool().get(ui, v); return s ? s->frame : 0u; }
u32 videoDecodedFrames(const UiContext& ui, NodeHandle v) { const VideoState* s = videoPool().get(ui, v); return s ? s->decoded : 0u; }

} // namespace fuse::ui::widgets
