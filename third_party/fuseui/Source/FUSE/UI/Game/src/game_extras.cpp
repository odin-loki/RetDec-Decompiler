// FUSE UI7 game UI, second part. See game_extras.hpp.
#include "fuse/ui/game/game_extras.hpp"

#include "fuse/ui/widgets/widgets.hpp"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace fuse::ui::game {
namespace {

constexpr f32 kPi = 3.14159265358979f;

NodeHandle make(UiContext& ui, std::string_view cls, NodeHandle parent, std::string_view id = {}) {
    auto h = ui.create(cls, parent, id);
    return h ? *h : NodeHandle{};
}
void px(UiContext& ui, NodeHandle n, Prop p, f32 v) { ui.set(n, p, PropValue::ofLength(Length::px(std::round(v)))); }
void setShown(UiContext& ui, NodeHandle n, bool on) { ui.set(n, Prop::Display, PropValue::ofBool(on)); }
f32 lo32(u64 v) { return std::bit_cast<f32>(u32(v)); }
f32 hi32(u64 v) { return std::bit_cast<f32>(u32(v >> 32)); }
u64 pack(f32 lo, f32 hi) { return u64(std::bit_cast<u32>(lo)) | (u64(std::bit_cast<u32>(hi)) << 32); }
bool inside(Rect r, Vec2 p) { return p.x >= r.x && p.y >= r.y && p.x < r.x + r.w && p.y < r.y + r.h; }
void text(const PaintContext& pc, DrawList& dl, std::string_view s, Rect box, Color c, f32 size,
          TextAlignment al = TextAlignment::Center) {
    pc.ui.paintString(dl, s, box, pc.tint(c), size, al, pc.node.index);
}
void disc(DrawList& dl, Vec2 c, f32 r, Color fill, Color border = {}, f32 bw = 0.f) {
    dl.roundedRect({c.x - r, c.y - r, 2.f * r, 2.f * r}, fill, {r, r, r, r}, border, bw);
}

constexpr std::string_view kExtrasCss = R"(
invslot { width: 52px; height: 52px; background: #161A22E0; border-color: #3A3F4B; border-width: 1px; border-radius: 6px; color: #FFFFFF; font-size: 12px; }
invslot:hover { border-color: #7A8394; }
invslot:focus-visible { border-color: #F2C14E; border-width: 2px; }
.inv-grid { gap: 4px; padding: 6px; background: #0E1014C8; border-radius: 8px; }
.inv-row { flex-direction: row; gap: 4px; padding: 0px; background: transparent; }
.inv-ghost { pointer-events: none; opacity: 0.85; }
.inv-tip { pointer-events: none; background: #0B0D12F0; color: #E6E8EE; border-color: #F2C14E; border-width: 1px; border-radius: 4px; padding: 4px 8px; font-size: 13px; }
touchstick { pointer-events: none; }
touchbutton { pointer-events: none; color: #FFFFFF; font-size: 16px; }
spinner { width: 40px; height: 40px; }
.loading { left: 0px; top: 0px; width: 100%; height: 100%; layout: anchor; background: #07080B; padding: 0px; }
.loading-title { left: 64px; bottom: 120px; color: #FFFFFF; font-size: 28px; }
.loading-tip { left: 64px; right: 40%; bottom: 80px; color: #A8AEBB; font-size: 15px; }
.loading-bar { left: 64px; right: 64px; bottom: 44px; height: 6px; }
.loading-pct { right: 64px; bottom: 56px; color: #E6E8EE; font-size: 14px; }
.loading-spin { right: 64px; bottom: 112px; }
.richdlg { left: 20%; right: 20%; bottom: 24px; height: 104px; background: #10131AE6; border-color: #3A3F4B; border-width: 1px; border-radius: 8px; padding: 10px 16px; gap: 6px; }
.richdlg-speaker { color: #F2C14E; font-size: 15px; }
richtext { color: #E6E8EE; font-size: 17px; grow: 1; }
)";

// invslot: userData lo = item id, hi = count (as u32); background-end = item colour; text = short name
struct InvSlot {
    static constexpr std::string_view kName = "invslot";
    static constexpr A11yRole kRole = A11yRole::Button;
    static constexpr bool kFocusable = true;
    static constexpr bool kInteractive = true;
    static Size measure(UiContext&, NodeHandle, Size) { return {52.f, 52.f}; }
    static void paint(const PaintContext& pc, DrawList& dl) {
        const u64 ud = pc.ui.userData(pc.node);
        const u32 item = u32(ud), count = u32(ud >> 32);
        if (item == 0 || count == 0) return;
        const Rect r = pc.content;
        const Rect icon{r.x + 6.f, r.y + 6.f, r.w - 12.f, r.h - 12.f};
        const Color c = pc.style.background2;
        dl.gradientRect(icon, pc.tint(c), pc.tint(Color{c.r * 0.55f, c.g * 0.55f, c.b * 0.55f, c.a}), true, {6, 6, 6, 6});
        const std::string_view s = pc.ui.text(pc.node);
        text(pc, dl, s.substr(0, std::min<std::size_t>(s.size(), 2)), icon, Color::rgb(0xFFFFFF), 16.f);
        if (count > 1) {
            char buf[12];
            std::snprintf(buf, sizeof buf, "%u", count);
            const Rect cr{r.x, r.bottom() - 16.f, r.w - 4.f, 14.f};
            text(pc, dl, buf, {cr.x + 1, cr.y + 1, cr.w, cr.h}, Color::rgb(0x000000, 0.9f), 12.f, TextAlignment::End);
            text(pc, dl, buf, cr, Color::rgb(0xFFFFFF), 12.f, TextAlignment::End);
        }
    }
};
// touchstick: userData = knob offset (px, packed); value = 1 while active
struct TouchStick {
    static constexpr std::string_view kName = "touchstick";
    static Size measure(UiContext&, NodeHandle, Size) { return {112.f, 112.f}; }
    static void paint(const PaintContext& pc, DrawList& dl) {
        const Rect r = pc.content;
        const Vec2 c{r.x + r.w * 0.5f, r.y + r.h * 0.5f};
        const f32 R = std::min(r.w, r.h) * 0.5f;
        const bool active = pc.ui.value(pc.node) > 0.5f;
        disc(dl, c, R, pc.tint(Color::rgb(0xFFFFFF, active ? 0.16f : 0.08f)), pc.tint(Color::rgb(0xFFFFFF, 0.45f)), 2.f);
        const u64 ud = pc.ui.userData(pc.node);
        disc(dl, {c.x + lo32(ud), c.y + hi32(ud)}, R * 0.42f, pc.tint(Color::rgb(0xFFFFFF, active ? 0.75f : 0.4f)));
    }
};
// touchbutton: userData = 1 while held
struct TouchButton {
    static constexpr std::string_view kName = "touchbutton";
    static Size measure(UiContext&, NodeHandle, Size) { return {64.f, 64.f}; }
    static void paint(const PaintContext& pc, DrawList& dl) {
        const Rect r = pc.content;
        const Vec2 c{r.x + r.w * 0.5f, r.y + r.h * 0.5f};
        const f32 R = std::min(r.w, r.h) * 0.5f;
        const bool held = pc.ui.userData(pc.node) != 0;
        disc(dl, c, R, pc.tint(Color::rgb(held ? 0x2B6CB0 : 0xFFFFFF, held ? 0.85f : 0.14f)), pc.tint(Color::rgb(0xFFFFFF, 0.5f)), 2.f);
        text(pc, dl, pc.ui.text(pc.node), r, pc.style.color, pc.style.fontSize);
    }
};
// spinner: userData lo = angle (radians)
struct Spinner {
    static constexpr std::string_view kName = "spinner";
    static Size measure(UiContext&, NodeHandle, Size) { return {40.f, 40.f}; }
    static void paint(const PaintContext& pc, DrawList& dl) {
        const Rect r = pc.content;
        const Vec2 c{r.x + r.w * 0.5f, r.y + r.h * 0.5f};
        const f32 R = std::min(r.w, r.h) * 0.5f - 2.f, a0 = lo32(pc.ui.userData(pc.node));
        for (int i = 0; i < 12; ++i) {
            const f32 a = a0 - f32(i) * (2.f * kPi / 12.f);
            const f32 k = 1.f - f32(i) / 12.f;
            dl.line({c.x + std::cos(a) * R * 0.45f, c.y + std::sin(a) * R * 0.45f}, {c.x + std::cos(a) * R, c.y + std::sin(a) * R},
                    pc.tint(Color::rgb(0xF2C14E, 0.15f + 0.85f * k)), 3.f);
        }
    }
};

// richtext: text = plain; spans come from the RichDialogue bound to the node; userData = visible bytes
struct RichBinding { const UiContext* ui; u32 node; const RichDialogue* dlg; };
std::vector<RichBinding>& richBindings() { static std::vector<RichBinding> v; return v; }
const RichDialogue* richOf(const UiContext& ui, u32 node) {
    for (const RichBinding& b : richBindings()) if (b.ui == &ui && b.node == node) return b.dlg;
    return nullptr;
}
struct RichTextW {
    static constexpr std::string_view kName = "richtext";
    static constexpr A11yRole kRole = A11yRole::Text;
    static Size measure(UiContext&, NodeHandle, Size a) { return {std::isfinite(a.w) ? a.w : 400.f, 48.f}; }
    static void paint(const PaintContext& pc, DrawList& dl) {
        const RichDialogue* d = richOf(pc.ui, pc.node.index);
        if (!d) return;
        const RichText& rt = d->text();
        const u32 vis = u32(pc.ui.userData(pc.node));
        const Rect r = pc.content;
        const f32 size = pc.style.fontSize, lineH = std::ceil(size * 1.35f);
        f32 x = r.x, y = r.y;
        const f32 t = f32(pc.ui.frameIndex() % 1024u);
        u32 wordIndex = 0;
        for (const RichSpan& sp : rt.spans) {
            u32 i = sp.begin;
            while (i < sp.end && i < vis) {
                // one word (plus its trailing spaces) at a time, wrapped at the content width
                u32 j = i;
                while (j < sp.end && rt.plain[j] != ' ' && rt.plain[j] != '\n') ++j;
                while (j < sp.end && rt.plain[j] == ' ') ++j;
                if (j == i) ++j; // newline
                const u32 shownEnd = std::min(j, vis);
                if (rt.plain[i] == '\n') { x = r.x; y += lineH; i = j; continue; }
                const std::string_view word(rt.plain.data() + i, j - i);
                const f32 w = pc.ui.measureString(word, size);
                if (x > r.x && x + pc.ui.measureString(std::string_view(word).substr(0, word.find_last_not_of(' ') + 1), size) > r.right()) {
                    x = r.x;
                    y += lineH;
                }
                const std::string_view shown(rt.plain.data() + i, shownEnd - i);
                f32 dy = 0.f, dx = sp.italic ? 1.f : 0.f;
                if (sp.shake) { dy = std::sin(t * 0.9f + f32(wordIndex) * 1.7f) * 1.5f; dx += std::cos(t * 1.3f + f32(wordIndex)) * 1.f; }
                const Rect box{x + dx, y + dy, w + 4.f, lineH};
                text(pc, dl, shown, box, sp.color, size, TextAlignment::Start);
                if (sp.bold) text(pc, dl, shown, {box.x + 0.75f, box.y, box.w, box.h}, sp.color, size, TextAlignment::Start);
                if (sp.italic) dl.rect({x, y + lineH - 3.f, pc.ui.measureString(shown, size), 1.f}, pc.tint(sp.color.withAlpha(0.35f)));
                x += w;
                i = j;
                ++wordIndex;
            }
        }
    }
};

bool namedColor(std::string_view n, Color& c) {
    struct Named { std::string_view n; u32 rgb; };
    static constexpr Named kNames[] = {{"red", 0xE5484D}, {"gold", 0xF2C14E}, {"blue", 0x5EA2FF}, {"green", 0x4CC26A},
                                       {"grey", 0x969CAA}, {"white", 0xFFFFFF}, {"purple", 0xB57CF2}};
    for (const Named& k : kNames) if (k.n == n) { c = Color::rgb(k.rgb); return true; }
    return parseColor(n, c);
}

} // namespace

void installExtras(UiContext& ui) {
    WidgetRegistry& r = ui.registry();
    if (r.find("invslot") != 0xFFFFFFFFu) return;
    r.registerWidget<InvSlot>();
    r.registerWidget<TouchStick>();
    r.registerWidget<TouchButton>();
    r.registerWidget<Spinner>();
    r.registerWidget<RichTextW>();
    (void)ui.addStyleSheet(kExtrasCss);
}

// ---- Inventory -----------------------------------------------------------------------------------------------------
Inventory::Inventory(UiContext& ui, const ItemCatalog& items, NodeHandle layer) : m_ui(ui), m_items(items), m_layer(layer) {
    installExtras(ui);
    if (!ui.alive(m_layer)) m_layer = ui.root();
    m_ghost = make(ui, "invslot", m_layer, "inv.ghost");
    ui.addClass(m_ghost, "inv-ghost");
    ui.setFocusable(m_ghost, false);
    setShown(ui, m_ghost, false);
    m_tip = widgets::label(ui, m_layer, "", "inv.tooltip");
    ui.addClass(m_tip, "inv-tip");
    setShown(ui, m_tip, false);
}

u32 Inventory::addGrid(NodeHandle parent, u32 cols, u32 rows, std::string_view id, u32 acceptMask) {
    Grid g;
    g.accept = acceptMask;
    g.node = make(m_ui, "panel", parent, id);
    m_ui.addClass(g.node, "inv-grid");
    for (u32 y = 0; y < rows; ++y) {
        const NodeHandle row = make(m_ui, "panel", g.node);
        m_ui.addClass(row, "inv-row");
        for (u32 x = 0; x < cols; ++x) {
            Slot s;
            s.node = make(m_ui, "invslot", row);
            g.slots.push_back(s);
        }
    }
    m_grids.push_back(std::move(g));
    return u32(m_grids.size() - 1);
}

void Inventory::refresh(u32 g, u32 s) {
    const Slot& sl = m_grids[g].slots[s];
    const NodeHandle n = sl.node;
    m_ui.setUserData(n, u64(sl.stack.item) | (u64(sl.stack.count) << 32));
    const ItemDef* d = m_items.get(sl.stack.item);
    m_ui.setText(n, d && sl.stack.count ? std::string_view(d->name) : std::string_view{});
    if (d) m_ui.set(n, Prop::Background2, PropValue::ofColor(d->color));
    m_ui.setA11y(n, A11yRole::Button, d && sl.stack.count ? std::string_view(d->name) : std::string_view("empty slot"));
    m_ui.invalidate(n);
}

void Inventory::set(u32 g, u32 s, ItemStack st) {
    if (st.count == 0) st = {};
    m_grids[g].slots[s].stack = st;
    refresh(g, s);
}

u32 Inventory::total(u32 item) const {
    u32 n = m_hand.item == item ? m_hand.count : 0u;
    for (const Grid& g : m_grids)
        for (const Slot& s : g.slots) n += s.stack.item == item ? s.stack.count : 0u;
    return n;
}

bool Inventory::find(NodeHandle n, u32& g, u32& s) const {
    for (u32 gi = 0; gi < m_grids.size(); ++gi)
        for (u32 si = 0; si < m_grids[gi].slots.size(); ++si)
            if (m_grids[gi].slots[si].node == n) { g = gi; s = si; return true; }
    return false;
}

bool Inventory::accepts(u32 g, ItemStack st) const {
    if (st.empty()) return true;
    const ItemDef* d = m_items.get(st.item);
    return d && (m_grids[g].accept >> (d->kind & 31u) & 1u);
}

void Inventory::pick(u32 g, u32 s) {
    m_hand = m_grids[g].slots[s].stack;
    m_fromGrid = g;
    m_fromSlot = s;
    set(g, s, {});
    const ItemDef* d = m_items.get(m_hand.item);
    m_ui.setUserData(m_ghost, u64(m_hand.item) | (u64(m_hand.count) << 32));
    m_ui.setText(m_ghost, d ? std::string_view(d->name) : std::string_view{});
    if (d) m_ui.set(m_ghost, Prop::Background2, PropValue::ofColor(d->color));
    setShown(m_ui, m_ghost, true);
    m_ui.invalidate(m_ghost);
}

void Inventory::returnHand() {
    if (m_hand.empty()) return;
    Slot& from = m_grids[m_fromGrid].slots[m_fromSlot];
    if (from.stack.empty()) set(m_fromGrid, m_fromSlot, m_hand);
    else { // origin was refilled by a swap: first free accepting slot
        bool placed = false;
        for (u32 g = 0; g < m_grids.size() && !placed; ++g)
            for (u32 s = 0; s < m_grids[g].slots.size() && !placed; ++s)
                if (m_grids[g].slots[s].stack.empty() && accepts(g, m_hand)) { set(g, s, m_hand); placed = true; }
    }
    m_hand = {};
    setShown(m_ui, m_ghost, false);
}

bool Inventory::place(u32 g, u32 s) {
    if (m_hand.empty()) return false;
    Slot& dst = m_grids[g].slots[s];
    if (!accepts(g, m_hand)) { ++m_rejects; return false; }
    if (dst.stack.empty()) {
        set(g, s, m_hand);
    } else if (dst.stack.item == m_hand.item) {
        const ItemDef* d = m_items.get(m_hand.item);
        const u32 cap = d ? std::max(1u, d->maxStack) : 1u;
        const u32 room = cap > dst.stack.count ? cap - dst.stack.count : 0u;
        if (room == 0) { ++m_rejects; return false; }
        const u32 moved = std::min(room, m_hand.count);
        set(g, s, {dst.stack.item, dst.stack.count + moved});
        m_hand.count -= moved;
        if (m_hand.count > 0) { returnHand(); ++m_drops; return true; }
    } else {
        // swap: the displaced stack goes to the origin slot (which is empty while we hold its stack)
        if (!accepts(m_fromGrid, dst.stack) || !m_grids[m_fromGrid].slots[m_fromSlot].stack.empty()) { ++m_rejects; return false; }
        const ItemStack displaced = dst.stack;
        set(g, s, m_hand);
        set(m_fromGrid, m_fromSlot, displaced);
    }
    m_hand = {};
    setShown(m_ui, m_ghost, false);
    ++m_drops;
    return true;
}

bool Inventory::pointerDown(Vec2 p) {
    m_pointer = p;
    u32 g, s;
    if (!find(m_ui.hitTest(p), g, s) || m_grids[g].slots[s].stack.empty() || holding()) return false;
    m_pressArmed = true; // the drag starts after a few pixels (a click without movement only focuses)
    m_pressAt = p;
    m_fromGrid = g;
    m_fromSlot = s;
    m_ui.setFocus(m_grids[g].slots[s].node, FocusReason::Pointer);
    return true;
}
bool Inventory::pointerMove(Vec2 p) {
    m_pointer = p;
    if (m_pressArmed && !m_pointerDrag && std::hypot(p.x - m_pressAt.x, p.y - m_pressAt.y) >= 4.f) {
        m_pointerDrag = true;
        pick(m_fromGrid, m_fromSlot);
    }
    return m_pointerDrag;
}
bool Inventory::pointerUp(Vec2 p) {
    m_pointer = p;
    const bool was = m_pointerDrag;
    m_pressArmed = false;
    m_pointerDrag = false;
    if (!was) return false;
    u32 g, s;
    if (!find(m_ui.hitTest(p), g, s) || !place(g, s)) returnHand();
    return true;
}
bool Inventory::activate() {
    u32 g, s;
    if (!find(m_ui.focused(), g, s)) return false;
    if (!holding()) {
        if (m_grids[g].slots[s].stack.empty()) return false;
        pick(g, s);
        return true;
    }
    return place(g, s);
}
void Inventory::cancel() {
    m_pointerDrag = m_pressArmed = false;
    returnHand();
}

void Inventory::update() {
    // ghost: under the pointer while dragging, else next to the focused slot (gamepad carry)
    if (holding()) {
        Vec2 at = m_pointer;
        if (!m_pointerDrag && m_ui.alive(m_ui.focused())) {
            const Rect fr = m_ui.rect(m_ui.focused());
            at = {fr.x + fr.w * 0.75f, fr.y + fr.h * 0.25f};
        }
        px(m_ui, m_ghost, Prop::Left, at.x - Inventory::kSlot * 0.5f);
        px(m_ui, m_ghost, Prop::Top, at.y - Inventory::kSlot * 0.5f);
    }
    // tooltip: focused slot (gamepad / keyboard) wins over the hovered one
    NodeHandle target = m_ui.focusVisible() ? m_ui.focused() : m_ui.hovered();
    u32 g, s;
    if (holding() || !find(target, g, s) || m_grids[g].slots[s].stack.empty()) {
        if (!find(target, g, s) || holding() || m_grids[g].slots[s].stack.empty()) {
            target = m_ui.hovered();
            if (holding() || !find(target, g, s) || m_grids[g].slots[s].stack.empty()) { setShown(m_ui, m_tip, false); return; }
        }
    }
    const ItemStack st = m_grids[g].slots[s].stack;
    const ItemDef* d = m_items.get(st.item);
    char buf[96];
    std::snprintf(buf, sizeof buf, st.count > 1 ? "%s  x%u" : "%s", d ? d->name.c_str() : "?", st.count);
    m_ui.setText(m_tip, buf);
    const Rect r = m_ui.rect(m_grids[g].slots[s].node);
    const Size vp = m_ui.viewport();
    const f32 w = m_ui.measureString(buf, 13.f) + 18.f;
    f32 x = r.right() + 6.f, y = r.y;
    if (x + w > vp.w) x = r.x - 6.f - w;
    px(m_ui, m_tip, Prop::Left, x);
    px(m_ui, m_tip, Prop::Top, y);
    setShown(m_ui, m_tip, true);
}

// ---- TouchControls -------------------------------------------------------------------------------------------------
TouchControls::TouchControls(UiContext& ui, NodeHandle layer) : m_ui(ui), m_layer(layer) {
    installExtras(ui);
    if (!ui.alive(m_layer)) m_layer = ui.root();
}
u32 TouchControls::addStick(Rect zone, f32 radius, bool floating, f32 dead) {
    Stick s;
    s.node = make(m_ui, "touchstick", m_layer);
    s.zone = zone;
    s.radius = radius;
    s.dead = dead;
    s.floating = floating;
    s.base = {zone.x + radius + 16.f, zone.bottom() - radius - 16.f}; // rest position: bottom-left of the zone
    px(m_ui, s.node, Prop::Width, radius * 2.f);
    px(m_ui, s.node, Prop::Height, radius * 2.f);
    m_sticks.push_back(s);
    place(m_sticks.back());
    return u32(m_sticks.size() - 1);
}
u32 TouchControls::addButton(Vec2 c, f32 r, std::string_view label) {
    Button b;
    b.node = make(m_ui, "touchbutton", m_layer);
    b.c = c;
    b.r = r;
    m_ui.setText(b.node, label);
    px(m_ui, b.node, Prop::Left, c.x - r);
    px(m_ui, b.node, Prop::Top, c.y - r);
    px(m_ui, b.node, Prop::Width, 2.f * r);
    px(m_ui, b.node, Prop::Height, 2.f * r);
    m_buttons.push_back(b);
    return u32(m_buttons.size() - 1);
}
void TouchControls::place(Stick& s) {
    px(m_ui, s.node, Prop::Left, s.base.x - s.radius);
    px(m_ui, s.node, Prop::Top, s.base.y - s.radius);
    m_ui.setUserData(s.node, pack(s.knob.x, s.knob.y));
    m_ui.setValue(s.node, s.touch != kNone ? 1.f : 0.f);
    m_ui.invalidate(s.node);
}
void TouchControls::touchDown(u32 id, Vec2 p) {
    if (!m_visible) return;
    for (Button& b : m_buttons) {
        if (b.touch == kNone && std::hypot(p.x - b.c.x, p.y - b.c.y) <= b.r) {
            b.touch = id;
            b.edges |= 1u;
            m_ui.setUserData(b.node, 1);
            m_ui.invalidate(b.node);
            return;
        }
    }
    for (Stick& s : m_sticks) {
        if (s.touch == kNone && inside(s.zone, p)) {
            s.touch = id;
            if (s.floating) s.base = p;
            s.knob = {};
            touchMove(id, p);
            place(s);
            return;
        }
    }
}
void TouchControls::touchMove(u32 id, Vec2 p) {
    for (Stick& s : m_sticks) {
        if (s.touch != id) continue;
        Vec2 d{p.x - s.base.x, p.y - s.base.y};
        const f32 len = std::hypot(d.x, d.y);
        if (len > s.radius) d = {d.x * s.radius / len, d.y * s.radius / len};
        s.knob = d;
        place(s);
    }
}
void TouchControls::touchUp(u32 id) {
    for (Button& b : m_buttons) {
        if (b.touch == id) {
            b.touch = kNone;
            b.edges |= 2u;
            m_ui.setUserData(b.node, 0);
            m_ui.invalidate(b.node);
        }
    }
    for (Stick& s : m_sticks) {
        if (s.touch == id) {
            s.touch = kNone;
            s.knob = {};
            place(s);
        }
    }
}
Vec2 TouchControls::stick(u32 i) const {
    const Stick& s = m_sticks[i];
    if (s.touch == kNone) return {};
    Vec2 v{s.knob.x / s.radius, s.knob.y / s.radius};
    const f32 len = std::hypot(v.x, v.y);
    if (len <= s.dead) return {};
    const f32 k = (std::min(len, 1.f) - s.dead) / (1.f - s.dead) / len; // rescale so the dead zone edge maps to 0
    return {v.x * k, v.y * k};
}
void TouchControls::endFrame() {
    for (Button& b : m_buttons) b.edges = 0;
}
void TouchControls::setVisible(bool on) {
    if (m_visible == on) return;
    m_visible = on;
    if (!on) {
        for (Button& b : m_buttons) if (b.touch != kNone) touchUp(b.touch);
        for (Stick& s : m_sticks) if (s.touch != kNone) touchUp(s.touch);
    }
    for (const Stick& s : m_sticks) setShown(m_ui, s.node, on);
    for (const Button& b : m_buttons) setShown(m_ui, b.node, on);
}

// ---- LoadingScreen -------------------------------------------------------------------------------------------------
LoadingScreen::LoadingScreen(UiContext& ui, NodeHandle layer) : m_ui(ui) {
    installExtras(ui);
    if (!ui.alive(layer)) layer = ui.root();
    m_root = make(ui, "panel", layer, "loading");
    ui.addClass(m_root, "loading");
    ui.addClass(m_root, "ui-blocker");
    m_title = widgets::label(ui, m_root, "", "loading.title");
    ui.addClass(m_title, "loading-title");
    m_tipLabel = widgets::label(ui, m_root, "", "loading.tip");
    ui.addClass(m_tipLabel, "loading-tip");
    m_spin = make(ui, "spinner", m_root, "loading.spinner");
    ui.addClass(m_spin, "loading-spin");
    m_pct = widgets::label(ui, m_root, "0%", "loading.percent");
    ui.addClass(m_pct, "loading-pct");
    m_bar = widgets::progressBar(ui, m_root, 0.f, "loading.bar");
    ui.addClass(m_bar, "loading-bar");
    setShown(ui, m_root, false);
}
void LoadingScreen::show(std::string_view title, std::string_view tips, f32 fade) {
    m_plain = false;
    m_ui.setText(m_title, title);
    m_tips.clear();
    while (!tips.empty()) {
        const auto p = tips.find('\n');
        m_tips.emplace_back(tips.substr(0, p));
        tips = p == tips.npos ? std::string_view{} : tips.substr(p + 1);
    }
    m_tip = 0;
    m_tipTime = 0.f;
    m_ui.setText(m_tipLabel, m_tips.empty() ? std::string_view{} : std::string_view(m_tips[0]));
    m_target = m_shown = 0.f;
    m_finishing = false;
    m_fadeIn = std::max(fade, 1e-3f);
    m_phase = Phase::In;
    for (NodeHandle n : {m_title, m_tipLabel, m_spin, m_pct, m_bar}) setShown(m_ui, n, true);
    apply();
}
void LoadingScreen::setProgress(f32 p) { m_target = std::max(m_target, std::clamp(p, 0.f, 1.f)); }
void LoadingScreen::finish(f32 fade) {
    m_finishing = true;
    m_target = 1.f;
    m_fadeOut = std::max(fade, 1e-3f);
}
void LoadingScreen::fadeTo(f32 in, f32 hold, f32 out) {
    m_plain = true;
    for (NodeHandle n : {m_title, m_tipLabel, m_spin, m_pct, m_bar}) setShown(m_ui, n, false);
    m_fadeIn = std::max(in, 1e-3f);
    m_hold = hold;
    m_fadeOut = std::max(out, 1e-3f);
    m_phase = Phase::FadeIn;
    apply();
}
void LoadingScreen::update(f32 dt) {
    if (m_phase == Phase::Hidden) return;
    switch (m_phase) {
    case Phase::In:
    case Phase::FadeIn:
        m_alpha = std::min(1.f, m_alpha + dt / m_fadeIn);
        if (m_alpha >= 1.f) m_phase = m_phase == Phase::In ? Phase::Shown : Phase::FadeHold;
        break;
    case Phase::FadeHold:
        m_hold -= dt;
        if (m_hold <= 0.f) m_phase = Phase::Out;
        break;
    case Phase::Out:
        m_alpha = std::max(0.f, m_alpha - dt / m_fadeOut);
        if (m_alpha <= 0.f) m_phase = Phase::Hidden;
        break;
    default: break;
    }
    if (!m_plain) {
        // progress eases towards the target so a stalled stream still looks alive; spinner and tips run regardless
        m_shown += (m_target - m_shown) * std::min(1.f, dt * 6.f);
        if (m_target - m_shown < 0.002f) m_shown = m_target;
        m_angle = std::fmod(m_angle + dt * 2.f * kPi * 0.8f, 2.f * kPi);
        m_tipTime += dt;
        if (m_tips.size() > 1 && m_tipTime >= 4.f) {
            m_tipTime = 0.f;
            m_tip = (m_tip + 1) % u32(m_tips.size());
            m_ui.setText(m_tipLabel, m_tips[m_tip]);
        }
        if (m_finishing && m_phase == Phase::Shown && m_shown >= 1.f) m_phase = Phase::Out;
    }
    apply();
}
void LoadingScreen::apply() {
    setShown(m_ui, m_root, m_phase != Phase::Hidden);
    m_ui.set(m_root, Prop::Opacity, PropValue::ofNumber(m_alpha));
    if (m_plain) return;
    m_ui.setValue(m_bar, m_shown);
    char buf[8];
    std::snprintf(buf, sizeof buf, "%d%%", int(std::floor(m_shown * 100.f + 0.5f)));
    m_ui.setText(m_pct, buf);
    m_ui.setUserData(m_spin, pack(m_angle, 0.f));
    m_ui.invalidate(m_spin);
}

// ---- rich text -----------------------------------------------------------------------------------------------------
std::expected<RichText, std::string> parseRich(std::string_view m) {
    RichText out;
    struct Style { Color color = Color::rgb(0xE6E8EE); bool bold = false, italic = false, shake = false; };
    std::vector<std::pair<std::string, Style>> stack;
    Style cur;
    auto flush = [&](u32 from) {
        if (u32(out.plain.size()) > from) out.spans.push_back({from, u32(out.plain.size()), cur.color, cur.bold, cur.italic, cur.shake});
    };
    u32 spanStart = 0;
    auto err = [&](std::size_t at, const char* what) {
        char buf[96];
        std::snprintf(buf, sizeof buf, "rich text: %s at byte %zu", what, at);
        return std::unexpected(std::string(buf));
    };
    for (std::size_t i = 0; i < m.size();) {
        if (m[i] != '[') { out.plain.push_back(m[i++]); continue; }
        if (i + 1 < m.size() && m[i + 1] == '[') { out.plain.push_back('['); i += 2; continue; }
        const std::size_t close = m.find(']', i);
        if (close == m.npos) return err(i, "unterminated tag");
        std::string_view tag = m.substr(i + 1, close - i - 1);
        std::string_view arg;
        if (const auto eq = tag.find('='); eq != tag.npos) { arg = tag.substr(eq + 1); tag = tag.substr(0, eq); }
        const u32 at = u32(out.plain.size());
        if (!tag.empty() && tag[0] == '/') {
            const std::string_view name = tag.substr(1);
            if (stack.empty() || stack.back().first != name) return err(i, "mismatched closing tag");
            flush(spanStart);
            spanStart = at;
            cur = stack.back().second;
            stack.pop_back();
        } else if (tag == "b" || tag == "i" || tag == "shake" || tag == "color") {
            Style next = cur;
            if (tag == "b") next.bold = true;
            else if (tag == "i") next.italic = true;
            else if (tag == "shake") next.shake = true;
            else if (!namedColor(arg, next.color)) return err(i, "bad colour");
            flush(spanStart);
            spanStart = at;
            stack.emplace_back(std::string(tag), cur);
            cur = next;
        } else if (tag == "pause" || tag == "t") {
            char buf[32] = {};
            std::memcpy(buf, arg.data(), std::min<std::size_t>(arg.size(), 31));
            char* end = nullptr;
            const f32 v = std::strtof(buf, &end);
            if (arg.empty() || end != buf + std::min<std::size_t>(arg.size(), 31) || !(v >= 0.f)) return err(i, "bad number");
            if (tag == "pause") out.pauses.emplace_back(at, v);
            else {
                if (!out.cues.empty() && out.cues.back().second > v) return err(i, "voice cues must ascend");
                out.cues.emplace_back(at, v);
            }
        } else if (tag == "event") {
            if (arg.empty()) return err(i, "event needs a name");
            out.events.emplace_back(at, std::string(arg));
        } else {
            return err(i, "unknown tag");
        }
        i = close + 1;
    }
    if (!stack.empty()) return err(m.size(), "unclosed tag");
    flush(spanStart);
    return out;
}

RichDialogue::RichDialogue(UiContext& ui, NodeHandle parent) : m_ui(ui) {
    installExtras(ui);
    if (!ui.alive(parent)) parent = ui.root();
    m_box = make(ui, "panel", parent, "richdlg");
    ui.addClass(m_box, "richdlg");
    ui.addClass(m_box, "ui-blocker");
    m_speaker = widgets::label(ui, m_box, "", "richdlg.speaker");
    ui.addClass(m_speaker, "richdlg-speaker");
    m_body = make(ui, "richtext", m_box, "richdlg.body");
    richBindings().push_back({&ui, m_body.index, this});
    setShown(ui, m_box, false);
}
RichDialogue::~RichDialogue() {
    auto& v = richBindings();
    v.erase(std::remove_if(v.begin(), v.end(), [&](const RichBinding& b) { return b.dlg == this; }), v.end());
}
bool RichDialogue::start(std::string_view speaker, std::string_view markup) {
    auto parsed = parseRich(markup);
    if (!parsed) { m_error = parsed.error(); return false; }
    m_error.clear();
    m_text = std::move(*parsed);
    m_shown = 0;
    m_nextEvent = 0;
    m_acc = m_pause = m_time = 0.f;
    m_ui.setText(m_speaker, speaker);
    m_ui.setText(m_body, m_text.plain);
    m_ui.setA11y(m_body, A11yRole::Text, m_text.plain);
    m_open = true;
    setShown(m_ui, m_box, true);
    reveal(0);
    return true;
}
bool RichDialogue::say(std::string_view speaker, std::string_view markup, f32 cps) {
    m_cps = std::max(cps, 1.f);
    m_voiceLen = 0.f;
    return start(speaker, markup);
}
bool RichDialogue::sayVoiced(std::string_view speaker, std::string_view markup, f32 voiceSeconds) {
    m_voiceLen = std::max(voiceSeconds, 1e-3f);
    return start(speaker, markup);
}
void RichDialogue::reveal(std::size_t upto) {
    upto = std::min(upto, m_text.plain.size());
    // never split a UTF-8 sequence
    while (upto < m_text.plain.size() && (u8(m_text.plain[upto]) & 0xC0u) == 0x80u) ++upto;
    m_shown = std::max(m_shown, upto);
    while (m_nextEvent < m_text.events.size() && m_text.events[m_nextEvent].first <= m_shown) {
        if (m_hook) m_hook(m_hookUser, m_text.events[m_nextEvent].second);
        ++m_nextEvent;
    }
    m_ui.setUserData(m_body, m_shown);
    m_ui.invalidate(m_body);
}
void RichDialogue::update(f32 dt, f32 voiceTime) {
    if (!m_open || complete()) return;
    const std::size_t n = m_text.plain.size();
    if (m_voiceLen > 0.f) {
        m_time = voiceTime >= 0.f ? voiceTime : m_time + dt;
        // piecewise-linear map voice time -> byte offset through (0,0), the cues, (len, n)
        u32 b0 = 0;
        f32 t0 = 0.f;
        u32 b1 = u32(n);
        f32 t1 = m_voiceLen;
        for (const auto& [b, t] : m_text.cues) {
            if (t <= m_time) { b0 = b; t0 = t; }
            else { b1 = b; t1 = t; break; }
        }
        const f32 k = t1 > t0 ? std::clamp((m_time - t0) / (t1 - t0), 0.f, 1.f) : 1.f;
        reveal(b0 + std::size_t(std::floor(f32(b1 - b0) * k)));
        return;
    }
    if (m_pause > 0.f) {
        m_pause -= dt;
        if (m_pause > 0.f) return;
        dt = -m_pause;
        m_pause = 0.f;
    }
    m_acc += dt * m_cps;
    while (m_acc >= 1.f && !complete()) {
        m_acc -= 1.f;
        const std::size_t next = m_shown + 1;
        reveal(next);
        for (const auto& [b, s] : m_text.pauses) {
            if (b == m_shown && s > 0.f) { m_pause = s; m_acc = 0.f; return; }
        }
    }
}
void RichDialogue::skip() {
    if (!m_open) return;
    if (!complete()) { reveal(m_text.plain.size()); return; }
    m_open = false;
    setShown(m_ui, m_box, false);
}

// ---- sound theme ---------------------------------------------------------------------------------------------------
std::string_view soundName(UiSound s) {
    static constexpr std::string_view k[] = {"move", "accept", "back", "error", "open", "close"};
    return k[u32(s)];
}
std::string extractSoundTheme(std::string_view css, SoundTheme& out, std::vector<std::string>* errors) {
    std::string rest;
    for (;;) {
        const auto at = css.find("@sounds");
        if (at == css.npos) { rest.append(css); break; }
        rest.append(css.substr(0, at));
        const auto open = css.find('{', at), close = css.find('}', at);
        if (open == css.npos || close == css.npos || close < open) {
            if (errors) errors->push_back("@sounds: missing { }");
            break;
        }
        std::string_view body = css.substr(open + 1, close - open - 1);
        auto trim = [](std::string_view s) {
            while (!s.empty() && std::isspace(u8(s.front()))) s.remove_prefix(1);
            while (!s.empty() && std::isspace(u8(s.back()))) s.remove_suffix(1);
            if (s.size() >= 2 && (s.front() == '"' || s.front() == '\'') && s.back() == s.front()) s = s.substr(1, s.size() - 2);
            return s;
        };
        while (!body.empty()) {
            const auto semi = body.find(';');
            const std::string_view decl = body.substr(0, semi);
            body = semi == body.npos ? std::string_view{} : body.substr(semi + 1);
            const auto colon = decl.find(':');
            if (trim(decl).empty()) continue;
            if (colon == decl.npos) { if (errors) errors->push_back("@sounds: expected name: value"); continue; }
            const std::string_view k = trim(decl.substr(0, colon)), v = trim(decl.substr(colon + 1));
            bool known = false;
            for (u32 i = 0; i < 6; ++i)
                if (k == soundName(UiSound(i))) { out.clips[i] = std::string(v); known = true; }
            if (k == "volume") { out.volume = std::clamp(std::strtof(std::string(v).c_str(), nullptr), 0.f, 4.f); known = true; }
            if (!known && errors) errors->push_back("@sounds: unknown key " + std::string(k));
        }
        css = css.substr(close + 1);
    }
    return rest;
}

} // namespace fuse::ui::game
