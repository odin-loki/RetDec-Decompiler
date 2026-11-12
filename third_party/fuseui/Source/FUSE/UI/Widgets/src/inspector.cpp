// UI8 runtime UI inspector overlay (see inspector.hpp).
#include "fuse/ui/widgets/inspector.hpp"

#include "fuse/ui/widgets/widgets.hpp"

#include <cstdio>

namespace fuse::ui::widgets {
namespace {
void px(UiContext& ui, NodeHandle n, const char* prop, f32 v) {
    char b[32];
    std::snprintf(b, sizeof b, "%.1fpx", v);
    ui.setCss(n, prop, b);
}
std::string colorText(const Color& c) {
    char b[32];
    std::snprintf(b, sizeof b, "#%02X%02X%02X/%.2f", int(c.r * 255.f + 0.5f), int(c.g * 255.f + 0.5f), int(c.b * 255.f + 0.5f), c.a);
    return b;
}
} // namespace

void Inspector::setEnabled(bool on) {
    if (on == enabled()) return;
    if (!on) {
        if (m_ui.alive(m_root)) m_ui.destroy(m_root);
        m_root = m_hoverBox = m_selBox = m_panel = m_title = m_body = {};
        m_hover = m_selected = {};
        m_shown.clear();
        return;
    }
    const NodeHandle layer = overlayLayer(m_ui);
    m_root = *m_ui.create("panel", layer, "__inspector");
    m_ui.setCss(m_root, "layout", "absolute");
    for (const char* p : {"left", "top"}) m_ui.setCss(m_root, p, "0px");
    m_ui.setCss(m_root, "width", "100%");
    m_ui.setCss(m_root, "height", "100%");
    m_ui.setCss(m_root, "pointer-events", "none");
    auto box = [&](const char* border, const char* fill) {
        const NodeHandle b = *m_ui.create("panel", m_root);
        m_ui.setCss(b, "border-width", "2px");
        m_ui.setCss(b, "border-color", border);
        m_ui.setCss(b, "background", fill);
        m_ui.setCss(b, "pointer-events", "none");
        m_ui.setCss(b, "display", "none");
        return b;
    };
    m_hoverBox = box("#4FC3F7", "rgba(79,195,247,0.12)");
    m_selBox = box("#FF4FA3", "rgba(255,79,163,0.18)");
    m_panel = *m_ui.create("panel", m_root);
    m_ui.setCss(m_panel, "background", "rgba(14,16,22,0.94)");
    m_ui.setCss(m_panel, "border-width", "1px");
    m_ui.setCss(m_panel, "border-color", "#FF4FA3");
    m_ui.setCss(m_panel, "border-radius", "6px");
    m_ui.setCss(m_panel, "padding", "8px");
    m_ui.setCss(m_panel, "gap", "4px");
    m_ui.setCss(m_panel, "width", "300px");
    m_ui.setCss(m_panel, "pointer-events", "none");
    m_title = *m_ui.create("label", m_panel);
    m_ui.setCss(m_title, "color", "#FF4FA3");
    m_ui.setCss(m_title, "font-size", "13px");
    m_ui.setText(m_title, "UI Inspector");
    m_body = *m_ui.create("label", m_panel);
    m_ui.setCss(m_body, "color", "#E6E9F0");
    m_ui.setCss(m_body, "font-size", "11px");
    m_ui.setCss(m_body, "pointer-events", "none");
    m_ui.setCss(m_title, "pointer-events", "none");
    m_shown.clear();
    update();
}

bool Inspector::owns(NodeHandle n) const {
    for (NodeHandle h = n; m_ui.alive(h); h = m_ui.parent(h))
        if (h == m_root) return true;
    return false;
}

NodeHandle Inspector::pick(Vec2 p) const {
    // depth-first, last child on top; skip the overlay layer (inspector, popups) and hidden subtrees
    NodeHandle best;
    auto visit = [&](auto&& self, NodeHandle n) -> void {
        if (!m_ui.computed(n).visible) return;
        const Rect r = m_ui.rect(n);
        const bool inside = p.x >= r.x && p.y >= r.y && p.x < r.x + r.w && p.y < r.y + r.h;
        if (inside) best = n;
        for (NodeHandle c = m_ui.firstChild(n); m_ui.alive(c); c = m_ui.nextSibling(c)) {
            if (m_ui.idOf(c) == "__inspector" || m_ui.registry().get(m_ui.classOf(c)).name == "layer") continue;
            self(self, c);
        }
    };
    visit(visit, m_ui.root());
    return best;
}

void Inspector::select(NodeHandle n) {
    m_selected = m_ui.alive(n) && !owns(n) ? n : NodeHandle{};
    update();
}

bool Inspector::pointerMove(Vec2 p) {
    if (!enabled()) return false;
    m_hover = pick(p);
    update();
    return false;
}
bool Inspector::pointerDown(Vec2 p) {
    if (!enabled()) return false;
    select(pick(p));
    return true;
}
bool Inspector::keyDown(Key k) {
    if (!enabled()) return false;
    NodeHandle s = m_selected;
    switch (k) {
    case Key::Escape: setEnabled(false); return true;
    case Key::Up: if (s && m_ui.parent(s)) s = m_ui.parent(s); break;
    case Key::Down: if (s && m_ui.firstChild(s)) s = m_ui.firstChild(s); break;
    case Key::Right: if (s && m_ui.nextSibling(s)) s = m_ui.nextSibling(s); break;
    case Key::Left:
        if (s && m_ui.parent(s)) {
            NodeHandle prev;
            for (NodeHandle c = m_ui.firstChild(m_ui.parent(s)); m_ui.alive(c) && c != s; c = m_ui.nextSibling(c)) prev = c;
            if (prev) s = prev;
        }
        break;
    default: return true;
    }
    if (s && !owns(s) && m_ui.registry().get(m_ui.classOf(s)).name != "layer") select(s);
    return true;
}

std::string Inspector::describe(NodeHandle n) const {
    if (!m_ui.alive(n)) return "(nothing selected)";
    std::string out;
    char b[160];
    const std::string_view cls = m_ui.registry().get(m_ui.classOf(n)).name;
    const std::string_view id = m_ui.idOf(n);
    out += std::string(cls);
    if (!id.empty()) out += "#" + std::string(id);
    out += '\n';
    // path from the root (classes, ids)
    std::string path;
    for (NodeHandle h = m_ui.parent(n); m_ui.alive(h); h = m_ui.parent(h)) {
        std::string seg(m_ui.registry().get(m_ui.classOf(h)).name);
        if (!m_ui.idOf(h).empty()) seg += "#" + std::string(m_ui.idOf(h));
        path = path.empty() ? seg : seg + " > " + path;
    }
    out += "path: " + path + "\n";
    const Rect r = m_ui.rect(n);
    std::snprintf(b, sizeof b, "box: %.0f, %.0f  %.0f x %.0f\n", r.x, r.y, r.w, r.h);
    out += b;
    std::snprintf(b, sizeof b, "role: %s  children: %u\n", a11yRoleName(m_ui.role(n)), m_ui.childCount(n));
    out += b;
    const u32 st = m_ui.states(n);
    out += "state:";
    if (!st) out += " -";
    if (st & kStateHover) out += " hover";
    if (st & kStatePressed) out += " pressed";
    if (st & kStateFocus) out += " focus";
    if (st & kStateDisabled) out += " disabled";
    if (st & kStateChecked) out += " checked";
    if (st & kStateSelected) out += " selected";
    out += '\n';
    if (!m_ui.text(n).empty()) {
        std::string t(m_ui.text(n).substr(0, 40));
        for (char& c : t) if (c == '\n') c = ' ';
        out += "text: \"" + t + "\"\n";
    }
    const ComputedStyle& cs = m_ui.computed(n);
    out += "background: " + colorText(cs.background) + "\n";
    out += "color: " + colorText(cs.color) + "\n";
    std::snprintf(b, sizeof b, "font-size: %.0fpx  opacity: %.2f\n", cs.fontSize, cs.opacity);
    out += b;
    std::snprintf(b, sizeof b, "padding: %.0f %.0f %.0f %.0f\n", cs.padding.t, cs.padding.r, cs.padding.b, cs.padding.l);
    out += b;
    const FrameStats& fs = m_ui.stats();
    std::snprintf(b, sizeof b, "frame: %u nodes, %u styles, %u draw cmds", m_ui.nodeCount(), m_ui.styles().tableSize(), fs.drawCmds);
    out += b;
    return out;
}

void Inspector::update() {
    if (!enabled()) return;
    auto place = [&](NodeHandle box, NodeHandle target) {
        if (!m_ui.alive(target)) { m_ui.set(box, Prop::Display, PropValue::ofBool(false)); return; }
        const Rect r = m_ui.rect(target);
        m_ui.set(box, Prop::Display, PropValue::ofBool(true));
        px(m_ui, box, "left", r.x);
        px(m_ui, box, "top", r.y);
        px(m_ui, box, "width", r.w);
        px(m_ui, box, "height", r.h);
    };
    place(m_hoverBox, m_hover != m_selected ? m_hover : NodeHandle{});
    place(m_selBox, m_selected);
    // panel: right side, or left when the selection sits on the right
    const Size vp = m_ui.viewport();
    const Rect sr = m_ui.alive(m_selected) ? m_ui.rect(m_selected) : Rect{};
    const bool left = sr.x + sr.w * 0.5f > vp.w * 0.5f;
    px(m_ui, m_panel, "left", left ? 8.f : vp.w - 308.f);
    px(m_ui, m_panel, "top", 8.f);
    const std::string text = describe(m_ui.alive(m_selected) ? m_selected : m_hover);
    if (text != m_shown) {
        m_ui.setText(m_body, text);
        m_shown = text;
    }
}

} // namespace fuse::ui::widgets
