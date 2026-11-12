// FUSE UI Tier B widgets (plan §5): overlay layer, popup scope, popup menu + combo box list, menu bar, modal dialog,
// tooltip, virtualised list view, tree view, tab bar / tab widget, scroll area.
#include "fuse/ui/widgets/widgets.hpp"
#include "widgets_internal.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace fuse::ui::widgets {
namespace {

constexpr std::string_view kLayerId = "__fuse_ui_layer";

u64 pack(NodeHandle h) { return (u64(h.generation) << 32) | u64(h.index); }
NodeHandle unpack(u64 v) { return NodeHandle{u32(v & 0xFFFFFFFFu), u32(v >> 32)}; }

NodeHandle make(UiContext& ui, std::string_view cls, NodeHandle parent, std::string_view id = {}) {
    auto h = ui.create(cls, parent, id);
    return h ? *h : NodeHandle{};
}
bool isClass(const UiContext& ui, NodeHandle n, std::string_view cls) {
    return ui.alive(n) && ui.classOf(n) == ui.registry().find(cls);
}
void setPx(UiContext& ui, NodeHandle n, Prop p, f32 v) { ui.set(n, p, PropValue::ofLength(Length::px(v))); }

f32 rowHeight(UiContext& ui, NodeHandle n) {
    const f32 px = ui.computed(n).fontSize;
    return std::ceil(ui.font() ? ui.font()->metrics(px).lineHeight() : px * 1.25f) + 6.f;
}
Color textCol(const PaintContext& pc) { return pc.tint(pc.style.color); }
Color accent(const PaintContext& pc) { return pc.tint(pc.style.background2.a > 0.f ? pc.style.background2 : Color::rgb(0x3D8BFD)); }
void labelAt(const PaintContext& pc, DrawList& dl, std::string_view s, Rect box, TextAlignment al = TextAlignment::Start) {
    pc.ui.paintString(dl, s, box, textCol(pc), pc.style.fontSize, al, pc.node.index);
}

// '\n'-separated items
u32 count(std::string_view s) { return s.empty() ? 0u : u32(std::count(s.begin(), s.end(), '\n') + 1); }
std::string_view nth(std::string_view s, u32 index) {
    std::size_t at = 0;
    for (u32 i = 0;; ++i) {
        const std::size_t nl = s.find('\n', at);
        if (i == index) return s.substr(at, nl == std::string_view::npos ? std::string_view::npos : nl - at);
        if (nl == std::string_view::npos) return {};
        at = nl + 1;
    }
}
/// Calls f(index, line) for each line from `first`; stops when f returns false.
template <class F>
void eachLine(std::string_view s, F&& f) {
    std::size_t at = 0;
    for (u32 i = 0; at <= s.size(); ++i) {
        const std::size_t nl = s.find('\n', at);
        const std::string_view line = s.substr(at, nl == std::string_view::npos ? std::string_view::npos : nl - at);
        if (!f(i, line)) return;
        if (nl == std::string_view::npos) return;
        at = nl + 1;
    }
}

// ---- popup scope -----------------------------------------------------------------------------------------------
NodeHandle scopeOf(const UiContext& ui, NodeHandle n) {
    const u32 cls = ui.registry().find("popup");
    while (ui.alive(n) && ui.classOf(n) != cls) n = ui.parent(n);
    return ui.alive(n) ? n : NodeHandle{};
}
NodeHandle firstFocusable(const UiContext& ui, NodeHandle n) {
    for (NodeHandle c = ui.firstChild(n); ui.alive(c); c = ui.nextSibling(c)) {
        if (ui.registry().get(ui.classOf(c)).focusable && !ui.disabled(c)) return c;
        const NodeHandle d = firstFocusable(ui, c);
        if (d.valid()) return d;
    }
    return {};
}
void destroyCmd(UiContext& ui, NodeHandle n, u64, u64) {
    if (ui.alive(n)) ui.destroy(n);
}

struct Popup {
    static constexpr std::string_view kName = "popup";
    static constexpr A11yRole kRole = A11yRole::Group;
    static ComputedStyle defaults() {
        ComputedStyle s;
        s.layout = LayoutKind::Anchor;
        return s;
    }
    static bool onEvent(EventContext& ec, const Event& e) {
        const bool modal = ec.ui.hasClass(ec.node, "modal");
        if (e.type == EventType::PointerDown && ec.phase == Phase::Target) {
            if (!modal) closePopup(ec.ui, ec.node); // click on the scrim: outside the panel
            else if (const NodeHandle f = firstFocusable(ec.ui, ec.node); f.valid()) ec.ui.setFocus(f, FocusReason::Pointer);
            return true; // a modal scrim keeps focus inside the dialog
        }
        if (e.type == EventType::KeyDown && e.key == Key::Escape) {
            closePopup(ec.ui, ec.node);
            return true;
        }
        return false;
    }
};

struct Menu {
    static constexpr std::string_view kName = "menu";
    static constexpr A11yRole kRole = A11yRole::List;
    // Up / Down / Home / End move between items (wrapping), independent of layout so it works on the opening frame.
    static bool onEvent(EventContext& ec, const Event& e) {
        if (e.type != EventType::KeyDown) return false;
        UiContext& ui = ec.ui;
        const u32 itemCls = ui.registry().find("menuitem");
        NodeHandle items[256];
        u32 n = 0, cur = 0xFFFFFFFFu;
        for (NodeHandle it = ui.firstChild(ec.node); ui.alive(it) && n < 256; it = ui.nextSibling(it)) {
            if (ui.classOf(it) != itemCls || ui.disabled(it)) continue;
            if (it == ui.focused()) cur = n;
            items[n++] = it;
        }
        if (!n) return false;
        u32 to;
        switch (e.key) {
        case Key::Down: to = cur == 0xFFFFFFFFu ? 0 : (cur + 1) % n; break;
        case Key::Up: to = cur == 0xFFFFFFFFu ? n - 1 : (cur + n - 1) % n; break;
        case Key::Home: to = 0; break;
        case Key::End: to = n - 1; break;
        default: return false;
        }
        ui.setFocus(items[to], FocusReason::Keyboard);
        return true;
    }
};
struct MenuItem { // text = "label" or "label\n<sub items, one tab less>" (a submenu); value = line index in the root list
    static constexpr std::string_view kName = "menuitem";
    static constexpr A11yRole kRole = A11yRole::ListItem;
    static constexpr bool kFocusable = true;
    static constexpr bool kInteractive = true;
    static std::string_view label(std::string_view t) { return t.substr(0, t.find('\n')); }
    static bool hasSub(std::string_view t) { return t.find('\n') != std::string_view::npos; }
    static Size measure(UiContext& ui, NodeHandle n, Size) {
        const std::string_view t = ui.text(n);
        return {std::ceil(ui.measureString(label(t), ui.computed(n).fontSize)) + (hasSub(t) ? 18.f : 0.f), rowHeight(ui, n) - 6.f};
    }
    static void paint(const PaintContext& pc, DrawList& dl) {
        const std::string_view t = pc.ui.text(pc.node);
        if (pc.ui.checked(pc.node)) dl.rect({pc.rect.x + 2.f, pc.rect.y + 3.f, 3.f, pc.rect.h - 6.f}, accent(pc));
        labelAt(pc, dl, label(t), pc.content);
        if (hasSub(t)) { // chevron
            const Rect c = pc.content;
            const f32 x = c.right() - 8.f, y = c.y + c.h * 0.5f;
            dl.line({x, y - 4.f}, {x + 4.f, y}, textCol(pc), 1.5f);
            dl.line({x + 4.f, y}, {x, y + 4.f}, textCol(pc), 1.5f);
        }
    }
    static void openSub(UiContext& ui, NodeHandle n) {
        const std::string_view t = ui.text(n);
        const Rect r = ui.rect(n);
        const NodeHandle sc = scopeOf(ui, n);
        const Rect panel = ui.alive(sc) ? ui.rect(ui.firstChild(sc)) : r;
        const std::string_view sub = t.substr(t.find('\n') + 1);
        f32 w = 0.f; // estimated panel width: open to the left when the right side has no room
        eachLine(sub, [&](u32, std::string_view l) {
            if (l.empty() || l[0] != '\t') w = std::max(w, ui.measureString(l, ui.computed(n).fontSize) + 66.f);
            return true;
        });
        const f32 x = panel.right() - 2.f + w > ui.popupArea().x + ui.popupArea().w ? std::max(ui.popupArea().x, panel.x + 2.f - w) : panel.right() - 2.f;
        detail::popupMenuAt(ui, n, sub, {x, r.y - 4.f}, u32(ui.value(n)) + 1u);
    }
    static bool onEvent(EventContext& ec, const Event& e) {
        if (e.type == EventType::PointerEnter) { ec.ui.setFocus(ec.node, FocusReason::Pointer); return false; }
        UiContext& ui = ec.ui;
        const NodeHandle scope = scopeOf(ui, ec.node);
        NodeHandle owner = ui.alive(scope) ? unpack(ui.userData(scope)) : NodeHandle{};
        if (e.type == EventType::KeyDown && ec.phase == Phase::Target) {
            if (e.key == Key::Right && hasSub(ui.text(ec.node))) { openSub(ui, ec.node); return true; }
            if (e.key == Key::Left && isClass(ui, owner, "menuitem")) { closePopup(ui, ec.node); return true; }
            return false;
        }
        if (e.type != EventType::Click || ec.phase != Phase::Target) return false;
        if (hasSub(ui.text(ec.node))) { openSub(ui, ec.node); return true; }
        const u32 idx = u32(ui.value(ec.node));
        const std::string_view text = ui.text(ec.node);
        if (ui.alive(owner) && isClass(ui, owner, "combobox")) {
            ui.setValue(owner, f32(idx), true);
            ui.emit(owner, SignalKind::TextChanged, {.text = text});
        }
        // a submenu pick closes the whole cascade and reports to the root owner
        NodeHandle sc = scope;
        while (ui.alive(sc)) {
            ui.emit(sc, SignalKind::Submitted, {.value = f32(idx), .text = text});
            owner = unpack(ui.userData(sc));
            closePopup(ui, sc);
            sc = isClass(ui, owner, "menuitem") ? scopeOf(ui, owner) : NodeHandle{};
        }
        if (ui.alive(owner)) ui.emit(owner, SignalKind::Submitted, {.value = f32(idx), .text = text});
        return true;
    }
};

// ---- menu bar ----------------------------------------------------------------------------------------------------
struct MenuBar {
    static constexpr std::string_view kName = "menubar";
    static constexpr A11yRole kRole = A11yRole::Group;
    static ComputedStyle defaults() {
        ComputedStyle s;
        s.direction = Direction::Row;
        return s;
    }
};
struct MenuButton { // text = "Title\nitem\nitem..."
    static constexpr std::string_view kName = "menubutton";
    static constexpr A11yRole kRole = A11yRole::Button;
    static constexpr bool kFocusable = true;
    static constexpr bool kInteractive = true;
    static Size measure(UiContext& ui, NodeHandle n, Size) {
        return {std::ceil(ui.measureString(nth(ui.text(n), 0), ui.computed(n).fontSize)), rowHeight(ui, n) - 6.f};
    }
    static void paint(const PaintContext& pc, DrawList& dl) {
        labelAt(pc, dl, nth(pc.ui.text(pc.node), 0), pc.content, TextAlignment::Center);
    }
    static bool onEvent(EventContext& ec, const Event& e) {
        if (ec.phase != Phase::Target) return false;
        if (e.type == EventType::Click || (e.type == EventType::KeyDown && e.key == Key::Down)) {
            const std::string_view t = ec.ui.text(ec.node);
            const std::size_t nl = t.find('\n');
            if (nl == std::string_view::npos) return true;
            const Rect r = ec.ui.rect(ec.node);
            popupMenu(ec.ui, ec.node, t.substr(nl + 1), {r.x, r.bottom()});
            return true;
        }
        return false;
    }
};

// ---- tooltip -----------------------------------------------------------------------------------------------------
struct Tooltip {
    static constexpr std::string_view kName = "tooltip";
    static constexpr A11yRole kRole = A11yRole::Text;
    static ComputedStyle defaults() {
        ComputedStyle s;
        s.pointerEvents = false;
        s.visible = false;
        return s;
    }
};

// ---- list view ---------------------------------------------------------------------------------------------------
struct ListView {
    static constexpr std::string_view kName = "listview";
    static constexpr A11yRole kRole = A11yRole::List;
    static constexpr bool kFocusable = true;
    static constexpr bool kInteractive = true;
    static Size measure(UiContext& ui, NodeHandle n, Size) {
        return {160.f, rowHeight(ui, n) * f32(std::clamp(count(ui.text(n)), 1u, 8u))};
    }
    static u32 visibleRows(UiContext& ui, NodeHandle n) {
        return std::max(1u, u32(ui.contentRect(n).h / rowHeight(ui, n)));
    }
    static void setFirst(UiContext& ui, NodeHandle n, i64 first) {
        const u32 c = count(ui.text(n)), vis = visibleRows(ui, n);
        const i64 mx = c > vis ? i64(c - vis) : 0;
        const u64 f = u64(std::clamp<i64>(first, 0, mx));
        if (f != ui.userData(n)) {
            ui.setUserData(n, f);
            ui.setValue(n, ui.value(n)); // repaint
            ui.setState(n, kStateHover, ui.states(n) & kStateHover);
        }
    }
    static void select(UiContext& ui, NodeHandle n, i64 idx) {
        const u32 c = count(ui.text(n));
        if (!c) return;
        idx = std::clamp<i64>(idx, 0, i64(c) - 1);
        const u32 vis = visibleRows(ui, n), first = u32(ui.userData(n));
        if (idx < i64(first)) setFirst(ui, n, idx);
        else if (idx >= i64(first + vis)) setFirst(ui, n, idx - i64(vis) + 1);
        if (i64(ui.value(n)) == idx) return;
        ui.setValue(n, f32(idx), true);
        ui.emit(n, SignalKind::TextChanged, {.text = nth(ui.text(n), u32(idx))});
    }
    static void paint(const PaintContext& pc, DrawList& dl) {
        UiContext& ui = pc.ui;
        const Rect c = pc.content;
        const f32 rh = rowHeight(ui, pc.node);
        const u32 first = u32(ui.userData(pc.node));
        const i64 sel = i64(ui.value(pc.node));
        const u32 vis = u32(std::ceil(c.h / rh));
        dl.pushClip(c);
        eachLine(ui.text(pc.node), [&](u32 i, std::string_view line) {
            if (i < first) return true;
            if (i >= first + vis) return false; // virtualised: rows past the view are never touched
            const Rect row{c.x, c.y + f32(i - first) * rh, c.w, rh};
            if (i64(i) == sel) dl.rect(row, accent(pc).withAlpha((pc.states & kStateFocus ? 0.55f : 0.3f) * pc.opacity), 3.f);
            labelAt(pc, dl, line, {row.x + 6.f, row.y + 3.f, row.w - 12.f, rh - 6.f});
            return true;
        });
        dl.popClip();
        const u32 total = count(ui.text(pc.node)), shown = u32(c.h / rh);
        if (total > shown && shown) { // scroll indicator
            const f32 th = std::max(12.f, c.h * f32(shown) / f32(total));
            const f32 ty = c.y + (c.h - th) * f32(first) / f32(total - shown);
            dl.rect({c.right() - 4.f, ty, 3.f, th}, textCol(pc).withAlpha(0.35f * pc.opacity), 1.5f);
        }
    }
    static bool onEvent(EventContext& ec, const Event& e) {
        if (ec.phase != Phase::Target) return false;
        UiContext& ui = ec.ui;
        const NodeHandle n = ec.node;
        const i64 cur = i64(ui.value(n));
        const u32 c = count(ui.text(n)), vis = visibleRows(ui, n);
        switch (e.type) {
        case EventType::PointerDown: {
            const i64 row = i64(ui.userData(n)) + i64((e.pos.y - ui.contentRect(n).y) / rowHeight(ui, n));
            if (row >= 0 && row < i64(c)) select(ui, n, row);
            return true;
        }
        case EventType::Wheel: setFirst(ui, n, i64(ui.userData(n)) - i64(std::lround(e.wheel.y * 3.f))); return true;
        case EventType::KeyDown:
            switch (e.key) {
            case Key::Up: select(ui, n, cur - 1); return true;
            case Key::Down: select(ui, n, cur + 1); return true;
            case Key::PageUp: select(ui, n, cur - i64(vis)); return true;
            case Key::PageDown: select(ui, n, cur + i64(vis)); return true;
            case Key::Home: select(ui, n, 0); return true;
            case Key::End: select(ui, n, i64(c) - 1); return true;
            case Key::Enter:
                if (cur >= 0) ui.emit(n, SignalKind::Submitted, {.value = f32(cur), .text = nth(ui.text(n), u32(cur))});
                return true;
            default: return false;
            }
        default: return false;
        }
    }
};

// ---- tree view ---------------------------------------------------------------------------------------------------
u32 depthOf(std::string_view line) {
    u32 d = 0;
    while (d < line.size() && line[d] == '\t') ++d;
    return d;
}
bool lineOpen(u64 collapsed, u32 i) { return i >= 64 || !(collapsed >> i & 1u); }
/// Calls f(row, line, depth, hasChildren, open, text) for each shown row; stops when f returns false.
template <class F>
void eachTreeRow(std::string_view s, u64 collapsed, F&& f) {
    // look-ahead of one line to know whether a line has children
    u32 skipBelow = 0xFFFFFFFFu, row = 0;
    std::string_view prev;
    u32 prevIdx = 0;
    bool havePrev = false, stop = false;
    auto flush = [&](u32 nextDepth) {
        if (!havePrev || stop) return;
        const u32 d = depthOf(prev);
        if (d > skipBelow) return;
        skipBelow = 0xFFFFFFFFu;
        const bool kids = nextDepth > d && nextDepth != 0xFFFFFFFFu;
        const bool open = lineOpen(collapsed, prevIdx);
        if (kids && !open) skipBelow = d;
        if (!f(row++, prevIdx, d, kids, open, prev.substr(d))) stop = true;
    };
    eachLine(s, [&](u32 i, std::string_view line) {
        flush(depthOf(line));
        prev = line;
        prevIdx = i;
        havePrev = true;
        return !stop;
    });
    flush(0xFFFFFFFFu);
}
struct TreeView {
    static constexpr std::string_view kName = "treeview";
    static constexpr A11yRole kRole = A11yRole::List;
    static constexpr bool kFocusable = true;
    static constexpr bool kInteractive = true;
    static constexpr f32 kIndent = 16.f;
    static Size measure(UiContext& ui, NodeHandle n, Size) {
        return {200.f, rowHeight(ui, n) * f32(std::clamp(treeVisibleRows(ui, n), 1u, 10u))};
    }
    static void paint(const PaintContext& pc, DrawList& dl) {
        UiContext& ui = pc.ui;
        const Rect c = pc.content;
        const f32 rh = rowHeight(ui, pc.node);
        const i64 sel = i64(ui.value(pc.node));
        dl.pushClip(c);
        eachTreeRow(ui.text(pc.node), ui.userData(pc.node), [&](u32 row, u32 line, u32 depth, bool kids, bool open, std::string_view t) {
            const Rect r{c.x, c.y + f32(row) * rh, c.w, rh};
            if (r.y > c.bottom()) return false;
            if (i64(line) == sel) dl.rect(r, accent(pc).withAlpha((pc.states & kStateFocus ? 0.55f : 0.3f) * pc.opacity), 3.f);
            const f32 x = r.x + 4.f + f32(depth) * kIndent, my = r.y + rh * 0.5f;
            if (kids) {
                const Color a = textCol(pc);
                if (open) { dl.line({x + 2.f, my - 2.f}, {x + 6.f, my + 2.f}, a, 1.5f); dl.line({x + 6.f, my + 2.f}, {x + 10.f, my - 2.f}, a, 1.5f); }
                else { dl.line({x + 4.f, my - 4.f}, {x + 8.f, my}, a, 1.5f); dl.line({x + 8.f, my}, {x + 4.f, my + 4.f}, a, 1.5f); }
            }
            labelAt(pc, dl, t, {x + 14.f, r.y + 3.f, c.right() - x - 14.f, rh - 6.f});
            return true;
        });
        dl.popClip();
    }
    struct Hit { i64 line = -1; u32 depth = 0; bool kids = false, open = false; i64 row = -1; };
    static Hit rowInfo(UiContext& ui, NodeHandle n, auto pred) {
        Hit h;
        eachTreeRow(ui.text(n), ui.userData(n), [&](u32 row, u32 line, u32 depth, bool kids, bool open, std::string_view) {
            if (!pred(row, line)) return true;
            h = {i64(line), depth, kids, open, i64(row)};
            return false;
        });
        return h;
    }
    static void select(UiContext& ui, NodeHandle n, i64 line) {
        if (line < 0 || i64(ui.value(n)) == line) return;
        ui.setValue(n, f32(line), true);
        std::string_view t = nth(ui.text(n), u32(line));
        ui.emit(n, SignalKind::TextChanged, {.text = t.substr(depthOf(t))});
    }
    static bool onEvent(EventContext& ec, const Event& e) {
        if (ec.phase != Phase::Target) return false;
        UiContext& ui = ec.ui;
        const NodeHandle n = ec.node;
        const i64 sel = i64(ui.value(n));
        if (e.type == EventType::PointerDown) {
            const Rect c = ui.contentRect(n);
            const i64 row = i64((e.pos.y - c.y) / rowHeight(ui, n));
            const Hit h = rowInfo(ui, n, [&](u32 r, u32) { return i64(r) == row; });
            if (h.line < 0) return true;
            const f32 ax = c.x + 4.f + f32(h.depth) * kIndent;
            if (h.kids && e.pos.x >= ax && e.pos.x < ax + 14.f) setTreeExpanded(ui, n, u32(h.line), !h.open);
            else select(ui, n, h.line);
            return true;
        }
        if (e.type != EventType::KeyDown) return false;
        const Hit cur = rowInfo(ui, n, [&](u32, u32 l) { return i64(l) == sel; });
        switch (e.key) {
        case Key::Down: {
            const i64 want = cur.row + 1;
            select(ui, n, rowInfo(ui, n, [&](u32 r, u32) { return i64(r) == want; }).line);
            return true;
        }
        case Key::Up: {
            const i64 want = cur.row < 0 ? 0 : cur.row - 1;
            select(ui, n, rowInfo(ui, n, [&](u32 r, u32) { return i64(r) == want; }).line);
            return true;
        }
        case Key::Right: if (cur.kids && !cur.open) setTreeExpanded(ui, n, u32(cur.line), true); return true;
        case Key::Left:
            if (cur.kids && cur.open) setTreeExpanded(ui, n, u32(cur.line), false);
            else if (cur.depth > 0) { // to the parent
                i64 parent = -1;
                eachTreeRow(ui.text(n), ui.userData(n), [&](u32, u32 l, u32 d, bool, bool, std::string_view) {
                    if (i64(l) >= cur.line) return false;
                    if (d < cur.depth) parent = l;
                    return true;
                });
                select(ui, n, parent);
            }
            return true;
        case Key::Enter:
            if (sel >= 0) ui.emit(n, SignalKind::Submitted, {.value = f32(sel)});
            return true;
        default: return false;
        }
    }
};

// ---- tab bar -----------------------------------------------------------------------------------------------------
struct TabBar {
    static constexpr std::string_view kName = "tabbar";
    static constexpr A11yRole kRole = A11yRole::Tab;
    static constexpr bool kFocusable = true;
    static constexpr bool kInteractive = true;
    static constexpr f32 kPad = 12.f;
    static f32 tabW(UiContext& ui, NodeHandle n, std::string_view t) { return std::ceil(ui.measureString(t, ui.computed(n).fontSize)) + 2.f * kPad; }
    static Size measure(UiContext& ui, NodeHandle n, Size) {
        f32 w = 0.f;
        eachLine(ui.text(n), [&](u32, std::string_view t) { w += tabW(ui, n, t); return true; });
        return {w, rowHeight(ui, n)};
    }
    static void paint(const PaintContext& pc, DrawList& dl) {
        UiContext& ui = pc.ui;
        const Rect c = pc.content;
        const u32 sel = u32(ui.value(pc.node));
        f32 x = c.x;
        eachLine(ui.text(pc.node), [&](u32 i, std::string_view t) {
            const f32 w = tabW(ui, pc.node, t);
            if (i == sel) {
                dl.rect({x, c.y, w, c.h}, pc.tint(Color::rgb(0x34353E)), 4.f);
                dl.rect({x + 4.f, c.bottom() - 2.f, w - 8.f, 2.f}, accent(pc));
            }
            labelAt(pc, dl, t, {x, c.y, w, c.h}, TextAlignment::Center);
            x += w;
            return true;
        });
        dl.rect({c.x, c.bottom() - 1.f, c.w, 1.f}, pc.tint(Color::rgb(0x474955)));
    }
    static bool onEvent(EventContext& ec, const Event& e) {
        if (ec.phase != Phase::Target) return false;
        UiContext& ui = ec.ui;
        const NodeHandle n = ec.node;
        const u32 c = count(ui.text(n)), cur = u32(ui.value(n));
        auto pick = [&](u32 i) { if (c && i < c && i != cur) ui.setValue(n, f32(i), true); };
        if (e.type == EventType::PointerDown) {
            f32 x = ui.contentRect(n).x;
            eachLine(ui.text(n), [&](u32 i, std::string_view t) {
                const f32 w = tabW(ui, n, t);
                if (e.pos.x >= x && e.pos.x < x + w) { pick(i); return false; }
                x += w;
                return true;
            });
            return true;
        }
        if (e.type == EventType::KeyDown && e.key == Key::Right) { pick(cur + 1); return true; }
        if (e.type == EventType::KeyDown && e.key == Key::Left) { if (cur) pick(cur - 1); return true; }
        return false;
    }
};

void showPage(UiContext& ui, NodeHandle stack, u32 current) {
    u32 i = 0;
    for (NodeHandle p = ui.firstChild(stack); ui.alive(p); p = ui.nextSibling(p), ++i)
        ui.setCss(p, "display", i == current ? "flex" : "none");
}

} // namespace

// ---- registration --------------------------------------------------------------------------------------------------
void registerTierB(UiContext& ui) {
    WidgetRegistry& r = ui.registry();
    if (r.find("popup") != 0xFFFFFFFFu) return;
    r.registerWidget<Popup>();
    r.registerWidget<Menu>();
    r.registerWidget<MenuItem>();
    r.registerWidget<MenuBar>();
    r.registerWidget<MenuButton>();
    r.registerWidget<Tooltip>();
    r.registerWidget<ListView>();
    r.registerWidget<TreeView>();
    r.registerWidget<TabBar>();
    detail::registerTierB2(ui);
}

namespace detail {
bool openComboPopup(UiContext& ui, NodeHandle combo) {
    if (ui.registry().find("popup") == 0xFFFFFFFFu) return false;
    const Rect r = ui.rect(combo);
    const NodeHandle scope = popupMenu(ui, combo, ui.text(combo), {r.x, r.bottom() + 2.f});
    if (!ui.alive(scope)) return false;
    // check and focus the current item; the list is at least as wide as the box
    const NodeHandle menu = ui.firstChild(scope);
    setPx(ui, menu, Prop::MinWidth, r.w);
    const u32 cur = u32(ui.value(combo));
    for (NodeHandle it = ui.firstChild(menu); ui.alive(it); it = ui.nextSibling(it))
        if (isClass(ui, it, "menuitem") && u32(ui.value(it)) == cur) {
            ui.setChecked(it, true);
            ui.setFocus(it, FocusReason::Keyboard);
        }
    return true;
}
} // namespace detail

NodeHandle overlayLayer(UiContext& ui) {
    NodeHandle l = ui.find(kLayerId);
    if (!ui.alive(l)) return make(ui, "layer", ui.root(), kLayerId);
    if (ui.nextSibling(l).valid()) ui.moveTo(l, ui.root());
    return l;
}

namespace detail {
NodeHandle popupMenuAt(UiContext& ui, NodeHandle owner, std::string_view items, Vec2 at, u32 base, std::string_view id) {
    registerTierB(ui);
    const NodeHandle scope = make(ui, "popup", overlayLayer(ui), id);
    if (!ui.alive(scope)) return {};
    ui.setUserData(scope, pack(owner));
    for (Prop p : {Prop::Left, Prop::Top, Prop::Right, Prop::Bottom}) setPx(ui, scope, p, 0.f);
    const NodeHandle menu = make(ui, "menu", scope);
    // the popup layer clamps the measured panel into the viewport ("clamp" class); open upward when it would not fit
    ui.addClass(menu, "clamp");
    const Rect area = ui.popupArea(); // the screen with OS-window popups, the viewport otherwise
    u32 rows = 0;
    eachLine(items, [&](u32, std::string_view t) { rows += t.empty() || t[0] != '\t'; return true; });
    const f32 estH = f32(rows) * 26.f + 8.f;
    setPx(ui, menu, Prop::Left, at.x);
    setPx(ui, menu, Prop::Top, at.y + estH > area.y + area.h && at.y - estH >= area.y ? at.y - estH : at.y);
    NodeHandle firstItem, item;
    std::string sub; // creation-time only
    auto flush = [&] {
        if (ui.alive(item) && !sub.empty()) { sub.insert(0, "\n"); sub.insert(0, ui.text(item)); ui.setText(item, sub); }
        sub.clear();
    };
    eachLine(items, [&](u32 i, std::string_view t) {
        if (!t.empty() && t[0] == '\t') { // child of the previous item
            if (!sub.empty()) sub += '\n';
            sub += t.substr(1);
            return true;
        }
        flush();
        item = {};
        if (t == "-") { separator(ui, menu); return true; }
        item = make(ui, "menuitem", menu);
        ui.setText(item, t);
        ui.setRange(item, 0.f, 1e9f, 0.f);
        ui.setValue(item, f32(base + i));
        if (!firstItem.valid()) firstItem = item;
        return true;
    });
    flush();
    ui.setModal(scope);
    if (ui.alive(firstItem)) ui.setFocus(firstItem, FocusReason::Keyboard);
    return scope;
}
NodeHandle popupPanel(UiContext& ui, NodeHandle owner, Vec2 at) {
    registerTierB(ui);
    const NodeHandle scope = make(ui, "popup", overlayLayer(ui));
    if (!ui.alive(scope)) return {};
    ui.setUserData(scope, pack(owner));
    for (Prop p : {Prop::Left, Prop::Top, Prop::Right, Prop::Bottom}) setPx(ui, scope, p, 0.f);
    const NodeHandle card = panel(ui, scope);
    ui.addClass(card, "card");
    ui.addClass(card, "clamp");
    setPx(ui, card, Prop::Left, at.x);
    setPx(ui, card, Prop::Top, at.y);
    ui.setModal(scope);
    return card;
}
} // namespace detail

NodeHandle popupMenu(UiContext& ui, NodeHandle owner, std::string_view items, Vec2 at, std::string_view id) {
    return detail::popupMenuAt(ui, owner, items, at, 0u, id);
}

void closePopup(UiContext& ui, NodeHandle n) {
    const NodeHandle scope = scopeOf(ui, n);
    if (!ui.alive(scope)) return;
    ui.setModal({});
    const NodeHandle owner = unpack(ui.userData(scope));
    if (ui.hasClass(scope, "modal")) {
        ui.setCss(scope, "display", "none"); // dialogs persist
    } else {
        ui.setCss(scope, "display", "none");
        ui.post(&destroyCmd, scope);
    }
    // closing a submenu hands input back to the parent menu
    if (const NodeHandle parentScope = scopeOf(ui, owner); ui.alive(parentScope) && popupOpen(ui, parentScope)) ui.setModal(parentScope);
    if (ui.alive(owner)) ui.setFocus(owner, FocusReason::Programmatic);
    else ui.setFocus({}, FocusReason::Programmatic);
}

bool popupOpen(const UiContext& ui, NodeHandle scope) { return ui.alive(scope) && ui.computed(scope).visible; }

NodeHandle menuBar(UiContext& ui, NodeHandle parent, std::string_view id) {
    registerTierB(ui);
    return make(ui, "menubar", parent, id);
}
NodeHandle menuBarMenu(UiContext& ui, NodeHandle bar, std::string_view title, std::string_view items, std::string_view id) {
    const NodeHandle b = make(ui, "menubutton", bar, id);
    std::string t(title);
    t += '\n';
    t += items;
    ui.setText(b, t);
    ui.setA11y(b, A11yRole::Button, title);
    return b;
}

NodeHandle dialog(UiContext& ui, std::string_view title, std::string_view id) {
    registerTierB(ui);
    const NodeHandle scope = make(ui, "popup", overlayLayer(ui), id);
    ui.addClass(scope, "modal");
    for (Prop p : {Prop::Left, Prop::Top, Prop::Right, Prop::Bottom}) setPx(ui, scope, p, 0.f);
    ui.setCss(scope, "display", "none");
    const NodeHandle card = panel(ui, scope);
    ui.addClass(card, "card");
    ui.addClass(card, "dialog");
    ui.set(card, Prop::CenterX, PropValue::ofBool(true));
    ui.set(card, Prop::CenterY, PropValue::ofBool(true));
    ui.setA11y(card, A11yRole::Window, title);
    const NodeHandle t = label(ui, card, title);
    ui.addClass(t, "title");
    panel(ui, card); // body
    return card;
}
NodeHandle dialogBody(const UiContext& ui, NodeHandle dlg) {
    const NodeHandle t = ui.firstChild(dlg);
    return ui.alive(t) ? ui.nextSibling(t) : NodeHandle{};
}
void openDialog(UiContext& ui, NodeHandle dlg) {
    const NodeHandle scope = ui.parent(dlg);
    if (!ui.alive(scope)) return;
    ui.setUserData(scope, pack(ui.focused()));
    overlayLayer(ui);
    ui.moveTo(scope, ui.parent(scope)); // topmost popup
    ui.setCss(scope, "display", "flex");
    ui.setModal(scope);
    const NodeHandle f = firstFocusable(ui, scope); // styles resolve on the next update: pick the first one directly
    if (f.valid()) ui.setFocus(f, FocusReason::Keyboard);
}
void closeDialog(UiContext& ui, NodeHandle dlg) { closePopup(ui, dlg); }

NodeHandle setTooltip(UiContext& ui, NodeHandle node, std::string_view text, f32 delay) {
    registerTierB(ui);
    const NodeHandle tip = make(ui, "tooltip", overlayLayer(ui));
    ui.setText(tip, text);
    ui.addClass(tip, "tooltip");
    ui.connect(node, SignalKind::HoverChanged, [tip, delay](UiContext& c, NodeHandle n, const SignalArgs& a) {
        if (!c.alive(tip)) return;
        if (!a.flag) { c.setCss(tip, "display", "none"); return; }
        c.startTimer(delay, false, [tip, n](UiContext& cx) {
            if (!cx.alive(tip) || !cx.alive(n) || cx.hovered().index != n.index) return;
            const Rect r = cx.rect(n);
            setPx(cx, tip, Prop::Left, r.x);
            setPx(cx, tip, Prop::Top, r.bottom() + 4.f);
            overlayLayer(cx);
            cx.setCss(tip, "display", "flex");
        });
    });
    return tip;
}

NodeHandle listView(UiContext& ui, NodeHandle parent, std::string_view items, i32 selected, std::string_view id) {
    registerTierB(ui);
    const NodeHandle n = make(ui, "listview", parent, id);
    ui.setText(n, items);
    ui.setRange(n, -1.f, 1e9f, 1.f);
    ui.setValue(n, f32(selected));
    return n;
}
std::string_view listItem(const UiContext& ui, NodeHandle list, u32 index) { return nth(ui.text(list), index); }
u32 listFirstVisible(const UiContext& ui, NodeHandle list) { return u32(ui.userData(list)); }

NodeHandle treeView(UiContext& ui, NodeHandle parent, std::string_view lines, std::string_view id) {
    registerTierB(ui);
    const NodeHandle n = make(ui, "treeview", parent, id);
    ui.setText(n, lines);
    ui.setRange(n, -1.f, 1e9f, 1.f);
    ui.setValue(n, -1.f);
    return n;
}
bool treeExpanded(const UiContext& ui, NodeHandle tree, u32 line) { return lineOpen(ui.userData(tree), line); }
void setTreeExpanded(UiContext& ui, NodeHandle tree, u32 line, bool open) {
    if (line >= 64 || treeExpanded(ui, tree, line) == open) return;
    u64 m = ui.userData(tree);
    m = open ? (m & ~(u64(1) << line)) : (m | (u64(1) << line));
    ui.setUserData(tree, m);
    ui.setText(tree, std::string(ui.text(tree))); // re-measure + repaint
    ui.emit(tree, SignalKind::Toggled, {.value = f32(line), .flag = open});
}
u32 treeVisibleRows(const UiContext& ui, NodeHandle tree) {
    u32 rows = 0;
    eachTreeRow(ui.text(tree), ui.userData(tree), [&](u32, u32, u32, bool, bool, std::string_view) { ++rows; return true; });
    return rows;
}

NodeHandle tabBar(UiContext& ui, NodeHandle parent, std::string_view titles, u32 current, std::string_view id) {
    registerTierB(ui);
    const NodeHandle n = make(ui, "tabbar", parent, id);
    ui.setText(n, titles);
    ui.setRange(n, 0.f, f32(std::max(1u, count(titles)) - 1), 1.f);
    ui.setValue(n, f32(current));
    return n;
}
NodeHandle tabWidget(UiContext& ui, NodeHandle parent, std::string_view titles, std::string_view id) {
    const NodeHandle w = panel(ui, parent, id);
    ui.addClass(w, "tabwidget");
    const NodeHandle bar = tabBar(ui, w, titles, 0);
    const NodeHandle stack = panel(ui, w);
    ui.setCss(stack, "flex-grow", "1");
    for (u32 i = 0, c = count(titles); i < c; ++i) {
        const NodeHandle p = panel(ui, stack);
        ui.setA11y(p, A11yRole::Group, nth(titles, i));
    }
    showPage(ui, stack, 0);
    ui.connect(bar, SignalKind::ValueChanged, [stack](UiContext& c, NodeHandle, const SignalArgs& a) { showPage(c, stack, u32(a.value)); });
    return w;
}
NodeHandle tabWidgetBar(const UiContext& ui, NodeHandle tabs) { return ui.firstChild(tabs); }
NodeHandle tabPage(const UiContext& ui, NodeHandle tabs, u32 index) {
    const NodeHandle bar = ui.firstChild(tabs);
    NodeHandle p = ui.alive(bar) ? ui.firstChild(ui.nextSibling(bar)) : NodeHandle{};
    for (u32 i = 0; i < index && ui.alive(p); ++i) p = ui.nextSibling(p);
    return ui.alive(p) ? p : NodeHandle{};
}

NodeHandle scrollArea(UiContext& ui, NodeHandle parent, std::string_view id) {
    const NodeHandle n = panel(ui, parent, id);
    ui.setCss(n, "overflow", "scroll");
    ui.setA11y(n, A11yRole::ScrollArea);
    return n;
}

} // namespace fuse::ui::widgets
