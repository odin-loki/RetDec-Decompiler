#include "fuse/ui/ui.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <limits>

namespace fuse::ui {

namespace {
constexpr u32 kNone = 0xFFFFFFFFu;
constexpr f32 kInf = std::numeric_limits<f32>::infinity();

enum NodeFlags : u32 {
    kFlagFocusable = 1u << 0,
    kFlagFocusScope = 1u << 2,
    kFlagRoleSet = 1u << 3,
};
enum Dirty : u8 { kDirtyStyle = 1, kDirtyLayout = 2, kDirtyPaint = 4, kDirtyText = 8, kDirtyMeasure = 16 };
} // namespace

const char* a11yRoleName(A11yRole r) {
    static const char* names[] = {"none", "window", "group", "text", "button", "checkbox", "radio", "switch",
                                  "slider", "progressbar", "textfield", "scrollarea", "list", "listitem", "image",
                                  "separator", "link", "combobox", "spinbox", "dial", "scrollbar", "tab"};
    return names[u32(r)];
}

struct UiContext::Node {
    u32 gen = 1;
    bool alive = false;
    u32 parent = kNone, first = kNone, last = kNone, next = kNone, prev = kNone;
    u32 cls = 0;
    u32 idHash = 0;
    std::array<u32, 6> classes{};
    u8 classCount = 0;
    u8 dirty = 0;
    u32 state = 0;
    u32 flags = 0;
    u32 style = 0;
    Rect rect, prevRect;
    Size measured{-1.f, -1.f}, measureKey{-2.f, -2.f};
    Vec2 scroll;
    Size extent;
    f32 value = 0.f, lo = 0.f, hi = 1.f, step = 0.f;
    i32 tabIndex = 0;
    A11yRole role = A11yRole::None;
    f32 animOpacity = 1.f;
    Vec2 offset;
    u64 user = 0;
};

struct UiContext::Cold {
    std::string id;
    std::string text;
    std::string a11yName;
    std::vector<std::pair<Prop, PropValue>> inl;
    fuse::text::TextLayout layout;
    f32 layoutWidth = -1.f, layoutSize = -1.f;
    std::unique_ptr<fuse::text::TextEditBuffer> edit;
};

// ---- Registry ------------------------------------------------------------------------------------------------------
u32 WidgetRegistry::add(WidgetClass c) {
    c.nameHash = hashId(c.name);
    for (u32 i = 0; i < m_classes.size(); ++i)
        if (m_classes[i].nameHash == c.nameHash) {
            m_classes[i] = std::move(c);
            return i;
        }
    m_classes.push_back(std::move(c));
    return u32(m_classes.size() - 1);
}
u32 WidgetRegistry::find(std::string_view name) const {
    const u32 h = hashId(name);
    for (u32 i = 0; i < m_classes.size(); ++i)
        if (m_classes[i].nameHash == h) return i;
    return kNone;
}

void registerCoreClasses(WidgetRegistry& r) {
    WidgetClass root;
    root.name = "root";
    root.role = A11yRole::Window;
    r.add(root);
    WidgetClass panel;
    panel.name = "panel";
    panel.role = A11yRole::None;
    r.add(panel);
    WidgetClass label;
    label.name = "label";
    label.role = A11yRole::Text;
    r.add(label);
    // Overlay layer (Tier B popups, menus, dialogs, tooltips): laid out over the whole viewport with Anchor layout,
    // ignored by its parent's measure and flow, transparent to the pointer itself. Keep it the root's last child.
    WidgetClass layer;
    layer.name = "layer";
    layer.role = A11yRole::None;
    layer.defaults.layout = LayoutKind::Anchor;
    layer.defaults.pointerEvents = false;
    r.add(layer);
}

// ---- Style subject -------------------------------------------------------------------------------------------------
struct ContextSubject final : StyleSubject {
    const UiContext& ui;
    const std::vector<UiContext::Node>& nodes;
    ContextSubject(const UiContext& u, const std::vector<UiContext::Node>& n) : ui(u), nodes(n) {}
    u32 typeHash(u32 n) const override { return ui.registry().get(nodes[n].cls).nameHash; }
    u32 idHash(u32 n) const override { return nodes[n].idHash; }
    bool hasClass(u32 n, u32 h) const override {
        for (u8 k = 0; k < nodes[n].classCount; ++k)
            if (nodes[n].classes[k] == h) return true;
        return false;
    }
    u32 states(u32 n) const override { return nodes[n].state; }
    u32 parent(u32 n) const override { return nodes[n].parent; }
};

// ---- Lifetime ------------------------------------------------------------------------------------------------------
UiContext::UiContext(const UiContextDesc& d) : m_font(d.font), m_atlas(d.atlasSize), m_viewport(d.viewport) {
    m_nodes.reserve(d.reserveNodes);
    m_cold.reserve(d.reserveNodes);
    m_draw.reserve(std::size_t(d.reserveNodes) * 4);
    m_path.reserve(256);
    m_scratchOrder.reserve(256);
    m_scratchTab.reserve(256);
    m_shapeScratch.reserve(256);
    m_connections.reserve(256);
    m_timers.reserve(64);
    m_anims.reserve(128);
    m_queue = std::make_unique<Command[]>(kQueueSize);
    for (u32 i = 0; i < kQueueSize; ++i) m_queue[i].seq.store(i, std::memory_order_relaxed);
    registerCoreClasses(m_registry);
    m_root = create(m_registry.find("root"), {}, "root");
    m_layerClass = m_registry.find("layer");
}

UiContext::~UiContext() = default;

UiContext::Node* UiContext::node(NodeHandle h) {
    if (h.index >= m_nodes.size()) return nullptr;
    Node& n = m_nodes[h.index];
    return (n.alive && n.gen == h.generation) ? &n : nullptr;
}
const UiContext::Node* UiContext::node(NodeHandle h) const {
    if (h.index >= m_nodes.size()) return nullptr;
    const Node& n = m_nodes[h.index];
    return (n.alive && n.gen == h.generation) ? &n : nullptr;
}
bool UiContext::alive(NodeHandle n) const { return node(n) != nullptr; }
NodeHandle UiContext::handleOf(u32 i) const {
    if (i >= m_nodes.size() || !m_nodes[i].alive) return {};
    return {i, m_nodes[i].gen};
}

u32 UiContext::alloc() {
    u32 i;
    if (!m_free.empty()) {
        i = m_free.back();
        m_free.pop_back();
        const u32 g = m_nodes[i].gen + 1;
        m_nodes[i] = Node{};
        m_nodes[i].gen = g;
        Cold& c = *m_cold[i];
        c.id.clear();
        c.text.clear();
        c.a11yName.clear();
        c.inl.clear();
        c.layoutWidth = c.layoutSize = -1.f;
        c.edit.reset();
    } else {
        i = u32(m_nodes.size());
        m_nodes.emplace_back();
        m_cold.push_back(std::make_unique<Cold>());
    }
    m_nodes[i].alive = true;
    ++m_live;
    return i;
}

void UiContext::linkBefore(u32 i, u32 p, u32 before) {
    Node& n = m_nodes[i];
    Node& pn = m_nodes[p];
    n.parent = p;
    if (before == kNone) {
        n.prev = pn.last;
        n.next = kNone;
        if (pn.last != kNone) m_nodes[pn.last].next = i;
        else pn.first = i;
        pn.last = i;
    } else {
        Node& b = m_nodes[before];
        n.next = before;
        n.prev = b.prev;
        if (b.prev != kNone) m_nodes[b.prev].next = i;
        else pn.first = i;
        b.prev = i;
    }
}

void UiContext::unlink(u32 i) {
    Node& n = m_nodes[i];
    if (n.parent == kNone) return;
    Node& p = m_nodes[n.parent];
    if (n.prev != kNone) m_nodes[n.prev].next = n.next;
    else p.first = n.next;
    if (n.next != kNone) m_nodes[n.next].prev = n.prev;
    else p.last = n.prev;
    n.parent = n.prev = n.next = kNone;
}

NodeHandle UiContext::create(u32 cls, NodeHandle parent, std::string_view id) {
    if (cls >= m_registry.size()) return {};
    if (parent.valid() && !node(parent)) return {};
    const u32 i = alloc();
    Node& n = m_nodes[i];
    n.cls = cls;
    const WidgetClass& wc = m_registry.get(cls);
    n.role = wc.role;
    if (wc.focusable) n.flags |= kFlagFocusable;
    if (!id.empty()) {
        n.idHash = hashId(id);
        m_cold[i]->id.assign(id);
    }
    n.dirty = kDirtyStyle | kDirtyLayout | kDirtyPaint | kDirtyText | kDirtyMeasure;
    if (parent.valid()) {
        linkBefore(i, parent.index, kNone);
        markLayout(parent.index);
    }
    m_anyStyleDirty = m_anyLayoutDirty = m_anyPaintDirty = true;
    return {i, n.gen};
}

std::expected<NodeHandle, UiError> UiContext::create(std::string_view className, NodeHandle parent, std::string_view id) {
    const u32 c = m_registry.find(className);
    if (c == kNone) return std::unexpected(UiError::UnknownClass);
    const NodeHandle h = create(c, parent, id);
    if (!h.valid()) return std::unexpected(UiError::InvalidHandle);
    return h;
}

void UiContext::destroyRec(u32 i) {
    for (u32 c = m_nodes[i].first; c != kNone;) {
        const u32 nx = m_nodes[c].next;
        destroyRec(c);
        c = nx;
    }
    Node& n = m_nodes[i];
    m_damage = m_damage.unite(n.rect);
    if (m_hover.index == i) m_hover = {};
    if (m_pressed.index == i) m_pressed = {};
    if (m_focus.index == i) m_focus = {};
    if (m_modal.index == i) m_modal = {};
    for (auto& a : m_anims)
        if (a.active && a.node.index == i) {
            a.active = false;
            --m_animCount;
        }
    n.alive = false;
    n.first = n.last = n.parent = n.next = n.prev = kNone;
    m_free.push_back(i);
    --m_live;
}

void UiContext::destroy(NodeHandle h) {
    Node* n = node(h);
    if (!n || h == m_root) return;
    const u32 p = n->parent;
    unlink(h.index);
    destroyRec(h.index);
    if (p != kNone) markLayout(p);
    m_anyPaintDirty = true;
}

void UiContext::moveTo(NodeHandle h, NodeHandle np, NodeHandle before) {
    Node* n = node(h);
    if (!n || !node(np) || h == m_root) return;
    for (u32 a = np.index; a != kNone; a = m_nodes[a].parent)
        if (a == h.index) return; // refuse cycles
    const u32 old = n->parent;
    const u32 want = node(before) ? before.index : kNone;
    if (old == np.index && n->next == want) return;
    unlink(h.index);
    linkBefore(h.index, np.index, want);
    if (old != kNone) markLayout(old);
    markLayout(np.index);
    markStyle(h.index);
}

NodeHandle UiContext::parent(NodeHandle h) const {
    const Node* n = node(h);
    return n ? handleOf(n->parent) : NodeHandle{};
}
NodeHandle UiContext::firstChild(NodeHandle h) const {
    const Node* n = node(h);
    return n ? handleOf(n->first) : NodeHandle{};
}
NodeHandle UiContext::nextSibling(NodeHandle h) const {
    const Node* n = node(h);
    return n ? handleOf(n->next) : NodeHandle{};
}
u32 UiContext::childCount(NodeHandle h) const {
    const Node* n = node(h);
    u32 c = 0;
    if (n)
        for (u32 k = n->first; k != kNone; k = m_nodes[k].next) ++c;
    return c;
}
NodeHandle UiContext::find(std::string_view id) const {
    const u32 h = hashId(id);
    for (u32 i = 0; i < m_nodes.size(); ++i)
        if (m_nodes[i].alive && m_nodes[i].idHash == h && m_cold[i]->id == id) return {i, m_nodes[i].gen};
    return {};
}
std::string_view UiContext::idOf(NodeHandle h) const {
    return node(h) ? std::string_view(m_cold[h.index]->id) : std::string_view{};
}
u32 UiContext::classOf(NodeHandle h) const { return node(h) ? node(h)->cls : kNone; }

// ---- Dirty tracking ------------------------------------------------------------------------------------------------
void UiContext::markLayout(u32 i) {
    m_anyLayoutDirty = m_anyPaintDirty = true;
    while (i != kNone) {
        Node& n = m_nodes[i];
        n.measureKey = {-2.f, -2.f};
        if (n.dirty & kDirtyMeasure) return; // ancestors already invalidated
        n.dirty |= kDirtyLayout | kDirtyPaint | kDirtyMeasure;
        const ComputedStyle& s = m_styles.style(n.style);
        // layout boundary: a fixed-size node's changes never propagate up (its parent re-arranges nothing)
        if (s.width.unit == Unit::Px && s.height.unit == Unit::Px && n.parent != kNone) {
            // ancestors still need to walk down to it: mark them "path" dirty without measure invalidation
            for (u32 a = n.parent; a != kNone; a = m_nodes[a].parent) {
                if (m_nodes[a].dirty & kDirtyLayout) break;
                m_nodes[a].dirty |= kDirtyLayout;
            }
            return;
        }
        i = n.parent;
    }
}
void UiContext::markStyle(u32 i) {
    m_nodes[i].dirty |= kDirtyStyle;
    m_anyStyleDirty = true;
}
void UiContext::markPaint(u32 i) {
    m_nodes[i].dirty |= kDirtyPaint;
    m_anyPaintDirty = true;
}
void UiContext::markAllDirty() {
    for (auto& n : m_nodes)
        if (n.alive) {
            n.dirty |= kDirtyStyle | kDirtyLayout | kDirtyPaint;
            n.measureKey = {-2.f, -2.f};
        }
    m_anyStyleDirty = m_anyLayoutDirty = m_anyPaintDirty = true;
}

// ---- Properties ----------------------------------------------------------------------------------------------------
void UiContext::addClass(NodeHandle h, std::string_view cls) {
    Node* n = node(h);
    if (!n) return;
    const u32 c = hashId(cls);
    for (u8 k = 0; k < n->classCount; ++k)
        if (n->classes[k] == c) return;
    if (n->classCount < n->classes.size()) {
        n->classes[n->classCount++] = c;
        markStyle(h.index);
        for (u32 k = n->first; k != kNone; k = m_nodes[k].next) markStyle(k);
    }
}
void UiContext::removeClass(NodeHandle h, std::string_view cls) {
    Node* n = node(h);
    if (!n) return;
    const u32 c = hashId(cls);
    for (u8 k = 0; k < n->classCount; ++k)
        if (n->classes[k] == c) {
            n->classes[k] = n->classes[--n->classCount];
            markStyle(h.index);
            for (u32 j = n->first; j != kNone; j = m_nodes[j].next) markStyle(j);
            return;
        }
}
bool UiContext::hasClass(NodeHandle h, std::string_view cls) const {
    const Node* n = node(h);
    if (!n) return false;
    const u32 c = hashId(cls);
    for (u8 k = 0; k < n->classCount; ++k)
        if (n->classes[k] == c) return true;
    return false;
}
void UiContext::set(NodeHandle h, Prop p, const PropValue& v) {
    if (!node(h)) return;
    auto& inl = m_cold[h.index]->inl;
    for (auto& kv : inl)
        if (kv.first == p) {
            kv.second = v;
            markStyle(h.index);
            return;
        }
    inl.emplace_back(p, v);
    markStyle(h.index);
}
bool UiContext::setCss(NodeHandle h, std::string_view property, std::string_view value) {
    Prop p;
    PropValue v;
    if (findProp(property, p)) {
        if (!parsePropValue(p, value, v)) return false;
        set(h, p, v);
        return true;
    }
    std::string css = "x{" + std::string(property) + ":" + std::string(value) + "}"; // shorthands
    auto sheet = StyleSheet::parse(css);
    if (!sheet || sheet->rules().empty() || sheet->rules()[0].decls.empty()) return false;
    for (const auto& d : sheet->rules()[0].decls) set(h, d.prop, d.value);
    return true;
}
void UiContext::clearInline(NodeHandle h) {
    if (!node(h)) return;
    m_cold[h.index]->inl.clear();
    markStyle(h.index);
}
void UiContext::setText(NodeHandle h, std::string_view s) {
    if (!node(h)) return;
    Cold& c = *m_cold[h.index];
    if (c.text == s) return;
    c.text.assign(s);
    c.layoutWidth = -1.f;
    m_nodes[h.index].dirty |= kDirtyText;
    markLayout(h.index);
}
std::string_view UiContext::text(NodeHandle h) const {
    return node(h) ? std::string_view(m_cold[h.index]->text) : std::string_view{};
}
void UiContext::setValue(NodeHandle h, f32 v, bool emitSignal) {
    Node* n = node(h);
    if (!n) return;
    if (n->step > 0.f) v = n->lo + std::round((v - n->lo) / n->step) * n->step;
    if (n->hi > n->lo) v = std::clamp(v, n->lo, n->hi);
    if (v == n->value) return;
    n->value = v;
    markPaint(h.index);
    if (emitSignal) emit(h, SignalKind::ValueChanged, {.value = v});
}
f32 UiContext::value(NodeHandle h) const { return node(h) ? node(h)->value : 0.f; }
void UiContext::setRange(NodeHandle h, f32 lo, f32 hi, f32 step) {
    Node* n = node(h);
    if (!n) return;
    n->lo = lo;
    n->hi = hi;
    n->step = step;
    const f32 v = n->value;
    n->value = std::numeric_limits<f32>::quiet_NaN();
    setValue(h, v);
}
f32 UiContext::rangeMin(NodeHandle h) const { return node(h) ? node(h)->lo : 0.f; }
f32 UiContext::rangeMax(NodeHandle h) const { return node(h) ? node(h)->hi : 0.f; }
f32 UiContext::rangeStep(NodeHandle h) const { return node(h) ? node(h)->step : 0.f; }
void UiContext::setState(NodeHandle h, u32 bits, bool on) {
    Node* n = node(h);
    if (!n) return;
    const u32 s = on ? (n->state | bits) : (n->state & ~bits);
    if (s == n->state) return;
    n->state = s;
    markStyle(h.index);
    markPaint(h.index);
    for (u32 k = n->first; k != kNone; k = m_nodes[k].next) markStyle(k);
}
u32 UiContext::states(NodeHandle h) const { return node(h) ? node(h)->state : 0u; }
void UiContext::setFocusable(NodeHandle h, bool on) {
    if (Node* n = node(h)) n->flags = on ? (n->flags | kFlagFocusable) : (n->flags & ~u32(kFlagFocusable));
}
void UiContext::setTabIndex(NodeHandle h, i32 i) {
    if (Node* n = node(h)) n->tabIndex = i;
}
void UiContext::setFocusScope(NodeHandle h, bool on) {
    if (Node* n = node(h)) n->flags = on ? (n->flags | kFlagFocusScope) : (n->flags & ~u32(kFlagFocusScope));
}
void UiContext::setA11y(NodeHandle h, A11yRole r, std::string_view name) {
    Node* n = node(h);
    if (!n) return;
    n->role = r;
    n->flags |= kFlagRoleSet;
    m_cold[h.index]->a11yName.assign(name);
}
A11yRole UiContext::role(NodeHandle h) const { return node(h) ? node(h)->role : A11yRole::None; }
void UiContext::setUserData(NodeHandle h, u64 v) {
    if (Node* n = node(h)) n->user = v;
}
u64 UiContext::userData(NodeHandle h) const { return node(h) ? node(h)->user : 0; }
fuse::text::TextEditBuffer& UiContext::editBuffer(NodeHandle h) {
    Cold& c = *m_cold[h.index];
    if (!c.edit) {
        c.edit = std::make_unique<fuse::text::TextEditBuffer>(256);
        c.edit->setText(c.text);
    }
    return *c.edit;
}
const fuse::text::TextLayout* UiContext::textLayout(NodeHandle h) const {
    return node(h) ? &m_cold[h.index]->layout : nullptr;
}
const ComputedStyle& UiContext::computed(NodeHandle h) const {
    const Node* n = node(h);
    return m_styles.style(n ? n->style : 0);
}
Rect UiContext::rect(NodeHandle h) const { return node(h) ? node(h)->rect : Rect{}; }
Rect UiContext::contentRect(NodeHandle h) const {
    const Node* n = node(h);
    if (!n) return {};
    const auto& p = m_styles.style(n->style).padding;
    return {n->rect.x + p.l, n->rect.y + p.t, std::max(0.f, n->rect.w - p.h()), std::max(0.f, n->rect.h - p.v())};
}
Vec2 UiContext::scroll(NodeHandle h) const { return node(h) ? node(h)->scroll : Vec2{}; }
Size UiContext::scrollExtent(NodeHandle h) const { return node(h) ? node(h)->extent : Size{}; }
void UiContext::scrollTo(NodeHandle h, Vec2 o) {
    Node* n = node(h);
    if (!n) return;
    const Rect c = contentRect(h);
    o.x = std::clamp(o.x, 0.f, std::max(0.f, n->extent.w - c.w));
    o.y = std::clamp(o.y, 0.f, std::max(0.f, n->extent.h - c.h));
    if (o == n->scroll) return;
    n->scroll = o;
    m_anyLayoutDirty = m_anyPaintDirty = true;
    for (u32 a = h.index; a != kNone; a = m_nodes[a].parent) {
        if (m_nodes[a].dirty & kDirtyLayout) break;
        m_nodes[a].dirty |= kDirtyLayout; // re-arrange path; measure caches stay valid
    }
}
void UiContext::setViewport(Size s) {
    if (s == m_viewport) return;
    m_viewport = s;
    markLayout(m_root.index);
}

// ---- Style pass ----------------------------------------------------------------------------------------------------
void UiContext::styleRec(u32 i, bool parentChanged) {
    Node& n = m_nodes[i];
    bool changed = false;
    if (parentChanged || (n.dirty & kDirtyStyle)) {
        const ContextSubject subj(*this, m_nodes);
        const ComputedStyle* ps = n.parent != kNone ? &m_styles.style(m_nodes[n.parent].style) : nullptr;
        const WidgetClass& wc = m_registry.get(n.cls);
        const u32 before = n.style;
        const u32 idx = m_styles.compute(subj, i, ps, &wc.defaults, m_cold[i]->inl);
        ++m_stats.styled;
        n.dirty &= u8(~kDirtyStyle);
        if (idx != before) {
            ComputedStyle la = m_styles.style(before);
            const ComputedStyle& lb = m_styles.style(idx);
            const bool fontChanged = la.fontSize != lb.fontSize || la.textAlign != lb.textAlign;
            n.style = idx;
            changed = true;
            markPaint(i);
            // compare layout-relevant fields only
            la.background = lb.background; la.background2 = lb.background2; la.borderColor = lb.borderColor;
            la.color = lb.color; la.shadowColor = lb.shadowColor; la.opacity = lb.opacity; la.radius = lb.radius;
            la.gradient = lb.gradient; la.shadowBlur = lb.shadowBlur; la.shadowX = lb.shadowX; la.shadowY = lb.shadowY; la.backdropBlur = lb.backdropBlur;
            la.pointerEvents = lb.pointerEvents; la.borderWidth = lb.borderWidth; la.textAlign = lb.textAlign;
            if (fontChanged) {
                m_cold[i]->layoutWidth = -1.f;
                n.dirty |= kDirtyText;
            }
            if (!(la == lb) || fontChanged) markLayout(i);
        }
    }
    for (u32 c = n.first; c != kNone; c = m_nodes[c].next) styleRec(c, changed);
}

void UiContext::stylePass() {
    if (m_styles.sheetVersion() != m_seenSheetVersion) {
        m_seenSheetVersion = m_styles.sheetVersion();
        for (auto& n : m_nodes)
            if (n.alive) n.dirty |= kDirtyStyle;
    }
    styleRec(m_root.index, false);
    m_anyStyleDirty = false;
}

// ---- Text ----------------------------------------------------------------------------------------------------------
void UiContext::ensureText(u32 i, f32 width) {
    Cold& c = *m_cold[i];
    const ComputedStyle& s = m_styles.style(m_nodes[i].style);
    if (!m_font) return;
    if (c.layoutWidth == width && c.layoutSize == s.fontSize && !(m_nodes[i].dirty & kDirtyText)) return;
    fuse::text::TextLayoutDesc d;
    d.pixelSize = s.fontSize;
    d.maxWidth = std::isfinite(width) ? width : 0.f;
    d.align = fuse::text::TextAlign(u8(s.textAlign));
    fuse::text::layoutText(*m_font, fuse::text::defaultShaper(), c.text, d, c.layout);
    c.layoutWidth = width;
    c.layoutSize = s.fontSize;
    m_nodes[i].dirty &= u8(~kDirtyText);
}

f32 UiContext::measureString(std::string_view s, f32 px) {
    if (!m_font) return 0.f;
    return fuse::text::measureText(*m_font, s, px, m_shapeScratch);
}

void UiContext::paintTextLayout(DrawList& dl, const fuse::text::TextLayout& tl, Vec2 o, Color col, f32 px, u32 nodeIdx) {
    if (!m_font || col.a <= 0.f) return;
    for (const auto& ln : tl.lines) {
        for (u32 k = ln.firstGlyph; k < ln.firstGlyph + ln.glyphCount; ++k) {
            const auto& g = tl.glyphs[k];
            if (g.glyph == fuse::text::kInvisibleGlyph) continue;
            fuse::text::forEachPaintGlyph(*m_font, g, px, [&](const fuse::text::Font& f, std::uint32_t glyph, f32 fpx,
                                                               const fuse::text::ColorLayer* layer) {
                const auto ag = m_atlas.get(f, glyph, fpx);
                if (!ag.valid || ag.w == 0) return;
                DrawCmd c;
                c.op = DrawOp::Glyph;
                c.rect = {std::round(o.x + g.x) + f32(ag.x0), std::round(o.y + ln.baseline) + f32(ag.y0), f32(ag.w), f32(ag.h)};
                c.u = ag.u;
                c.v = ag.v;
                c.tw = ag.w;
                c.th = ag.h;
                c.fill = (!layer || layer->foreground)
                             ? col
                             : Color{f32(layer->r) / 255.f, f32(layer->g) / 255.f, f32(layer->b) / 255.f, f32(layer->a) / 255.f * col.a};
                c.node = nodeIdx;
                dl.push(c);
            });
        }
    }
}

void UiContext::paintString(DrawList& dl, std::string_view s, Rect box, Color col, f32 px, TextAlignment align, u32 nodeIdx) {
    if (!m_font || s.empty()) return;
    fuse::text::TextLayoutDesc d;
    d.pixelSize = px;
    d.maxWidth = box.w;
    d.elide = true;
    d.align = fuse::text::TextAlign(u8(align));
    fuse::text::layoutText(*m_font, fuse::text::defaultShaper(), s, d, m_stringScratch);
    const f32 y = box.y + std::floor((box.h - m_stringScratch.height) * 0.5f);
    paintTextLayout(dl, m_stringScratch, {box.x, y}, col, px, nodeIdx);
}

// ---- Layout --------------------------------------------------------------------------------------------------------
namespace {
f32 clampSize(f32 v, const Length& lo, const Length& hi, f32 base) {
    if (hi.unit == Unit::Px || (hi.unit == Unit::Percent && std::isfinite(base))) v = std::min(v, hi.resolve(base, v));
    if (lo.unit == Unit::Px || (lo.unit == Unit::Percent && std::isfinite(base))) v = std::max(v, lo.resolve(base, v));
    return std::max(0.f, v);
}
bool fixedLen(const Length& l, f32 base) { return l.unit == Unit::Px || (l.unit == Unit::Percent && std::isfinite(base)); }
} // namespace

void UiContext::markArrange(u32 i) {
    m_anyLayoutDirty = m_anyPaintDirty = true;
    for (u32 a = i; a != kNone; a = m_nodes[a].parent) {
        if (m_nodes[a].dirty & kDirtyLayout) break;
        m_nodes[a].dirty |= kDirtyLayout;
    }
}

Size UiContext::measure(u32 i, Size avail) {
    Node& n = m_nodes[i];
    if (n.measureKey == avail && n.measured.w >= 0.f) return n.measured;
    ++m_stats.measured;
    n.dirty &= u8(~kDirtyMeasure);
    const ComputedStyle& s = m_styles.style(n.style);
    Size out{0.f, 0.f};
    if (s.visible) {
        const bool fw = fixedLen(s.width, avail.w), fh = fixedLen(s.height, avail.h);
        const f32 bw = fw ? s.width.resolve(avail.w, 0.f) : avail.w;
        const f32 bh = fh ? s.height.resolve(avail.h, 0.f) : avail.h;
        const Size in{std::max(0.f, bw - s.padding.h()), std::max(0.f, bh - s.padding.v())};
        Size c{0.f, 0.f};
        const WidgetClass& wc = m_registry.get(n.cls);
        if (wc.measure) {
            c = wc.measure(*this, {i, n.gen}, in);
        } else if (n.first != kNone) {
            const bool row = s.direction == Direction::Row;
            u32 count = 0;
            switch (s.layout) {
            case LayoutKind::Box:
                for (u32 k = n.first; k != kNone; k = m_nodes[k].next) {
                    const ComputedStyle& cs = m_styles.style(m_nodes[k].style);
                    if (!cs.visible || m_nodes[k].cls == m_layerClass) continue;
                    const Size m = measure(k, {in.w - cs.margin.h(), in.h - cs.margin.v()});
                    const f32 mw = m.w + cs.margin.h(), mh = m.h + cs.margin.v();
                    if (row) { c.w += mw; c.h = std::max(c.h, mh); }
                    else { c.h += mh; c.w = std::max(c.w, mw); }
                    ++count;
                }
                if (count > 1) (row ? c.w : c.h) += s.gap * f32(count - 1);
                break;
            case LayoutKind::Grid: {
                const u32 cols = std::max<u32>(1, s.gridColumnCount);
                std::array<f32, kMaxGridTracks> colW{};
                f32 rowH = 0.f, total = 0.f;
                u32 col = 0;
                const f32 cw = std::isfinite(in.w) ? (in.w - s.gap * f32(cols - 1)) / f32(cols) : kInf;
                for (u32 k = n.first; k != kNone; k = m_nodes[k].next) {
                    const ComputedStyle& cs = m_styles.style(m_nodes[k].style);
                    if (!cs.visible || m_nodes[k].cls == m_layerClass) continue;
                    const Size m = measure(k, {cw, kInf});
                    colW[col] = std::max(colW[col], m.w + cs.margin.h());
                    rowH = std::max(rowH, m.h + cs.margin.v());
                    if (++col == cols) { total += rowH + (total > 0.f ? s.gap : 0.f); rowH = 0.f; col = 0; }
                }
                if (col) total += rowH + (total > 0.f ? s.gap : 0.f);
                for (u32 k = 0; k < cols; ++k) {
                    const Length& t = s.gridColumnCount ? s.gridColumns[k] : Length{};
                    c.w += t.unit == Unit::Px ? t.value : colW[k];
                }
                c.w += s.gap * f32(cols - 1);
                c.h = total;
                break;
            }
            case LayoutKind::Flow: {
                f32 x = 0.f, y = 0.f, lineH = 0.f;
                for (u32 k = n.first; k != kNone; k = m_nodes[k].next) {
                    const ComputedStyle& cs = m_styles.style(m_nodes[k].style);
                    if (!cs.visible || m_nodes[k].cls == m_layerClass) continue;
                    const Size m = measure(k, {in.w, kInf});
                    const f32 mw = m.w + cs.margin.h(), mh = m.h + cs.margin.v();
                    if (x > 0.f && x + mw > in.w) { y += lineH + s.gap; x = 0.f; lineH = 0.f; }
                    x += mw + s.gap;
                    lineH = std::max(lineH, mh);
                    c.w = std::max(c.w, x - s.gap);
                }
                c.h = y + lineH;
                break;
            }
            default: // Stack, Anchor, Absolute: union of children
                for (u32 k = n.first; k != kNone; k = m_nodes[k].next) {
                    const ComputedStyle& cs = m_styles.style(m_nodes[k].style);
                    if (!cs.visible || m_nodes[k].cls == m_layerClass) continue;
                    const Size m = measure(k, in);
                    const f32 ox = s.layout == LayoutKind::Stack ? 0.f : cs.left.resolve(in.w, 0.f);
                    const f32 oy = s.layout == LayoutKind::Stack ? 0.f : cs.top.resolve(in.h, 0.f);
                    c.w = std::max(c.w, ox + m.w + cs.margin.h());
                    c.h = std::max(c.h, oy + m.h + cs.margin.v());
                }
                break;
            }
        }
        if (!wc.measure && !m_cold[i]->text.empty()) {
            ensureText(i, in.w);
            const auto& tl = m_cold[i]->layout;
            c.w = std::max(c.w, std::ceil(tl.width));
            c.h = std::max(c.h, std::ceil(tl.height));
        }
        out.w = fw ? bw : c.w + s.padding.h();
        out.h = fh ? bh : c.h + s.padding.v();
        out.w = clampSize(out.w, s.minWidth, s.maxWidth, avail.w);
        out.h = clampSize(out.h, s.minHeight, s.maxHeight, avail.h);
    }
    n.measured = out;
    n.measureKey = avail;
    return out;
}

void UiContext::arrange(u32 i, Rect box) {
    Node& n = m_nodes[i];
    if (box == n.rect && !(n.dirty & kDirtyLayout)) return;
    ++m_stats.arranged;
    if (!(box == n.rect)) {
        m_damage = m_damage.unite(n.rect).unite(box);
        n.prevRect = n.rect;
        n.rect = box;
        markPaint(i);
    }
    n.dirty &= u8(~(kDirtyLayout | kDirtyMeasure));
    arrangeChildren(i);
}

void UiContext::arrangeChildren(u32 i) {
    Node& n = m_nodes[i];
    const ComputedStyle& s = m_styles.style(n.style);
    const Rect c0 = contentRect({i, n.gen});
    if (!m_cold[i]->text.empty() && !m_registry.get(n.cls).measure) ensureText(i, c0.w);
    const Rect c{c0.x - n.scroll.x, c0.y - n.scroll.y, c0.w, c0.h};
    Size extent{0.f, 0.f};
    auto place = [&](u32 k, Rect r) {
        const Node& kn = m_nodes[k];
        r.x += kn.offset.x;
        r.y += kn.offset.y;
        extent.w = std::max(extent.w, r.right() - c.x - kn.offset.x + m_styles.style(kn.style).margin.r);
        extent.h = std::max(extent.h, r.bottom() - c.y - kn.offset.y + m_styles.style(kn.style).margin.b);
        arrange(k, r);
    };
    auto hidden = [&](u32 k) {
        if (m_nodes[k].cls == m_layerClass) { // overlay layer: covers the viewport, outside the flow
            arrange(k, {0.f, 0.f, m_viewport.w, m_viewport.h});
            return true;
        }
        if (m_styles.style(m_nodes[k].style).visible) return false;
        arrange(k, {c.x, c.y, 0.f, 0.f});
        return true;
    };
    switch (s.layout) {
    case LayoutKind::Box: {
        const bool row = s.direction == Direction::Row;
        const f32 mainAvail = row ? c.w : c.h, crossAvail = row ? c.h : c.w;
        f32 used = 0.f, growSum = 0.f, shrinkSum = 0.f;
        u32 count = 0;
        // pass 1: basis sizes (kept in the node's prevRect.w scratch? no: recompute; measure is cached)
        for (u32 k = n.first; k != kNone; k = m_nodes[k].next) {
            const ComputedStyle& cs = m_styles.style(m_nodes[k].style);
            if (!cs.visible || m_nodes[k].cls == m_layerClass) continue;
            const Size m = measure(k, {c.w - cs.margin.h(), c.h - cs.margin.v()});
            const f32 basis = fixedLen(cs.basis, mainAvail) ? cs.basis.resolve(mainAvail, 0.f) : (row ? m.w : m.h);
            used += basis + (row ? cs.margin.h() : cs.margin.v());
            growSum += cs.grow;
            shrinkSum += cs.shrink * basis;
            ++count;
        }
        if (count > 1) used += s.gap * f32(count - 1);
        const f32 free = mainAvail - used;
        f32 pos = 0.f, between = s.gap;
        if (free > 0.f && growSum <= 0.f) {
            switch (s.justify) {
            case Justify::Center: pos = free * 0.5f; break;
            case Justify::End: pos = free; break;
            case Justify::SpaceBetween: if (count > 1) between += free / f32(count - 1); break;
            case Justify::SpaceAround: between += free / f32(count); pos = free / f32(count) * 0.5f; break;
            default: break;
            }
        }
        for (u32 k = n.first; k != kNone; k = m_nodes[k].next) {
            if (hidden(k)) continue;
            const ComputedStyle& cs = m_styles.style(m_nodes[k].style);
            const Size m = measure(k, {c.w - cs.margin.h(), c.h - cs.margin.v()});
            f32 main = fixedLen(cs.basis, mainAvail) ? cs.basis.resolve(mainAvail, 0.f) : (row ? m.w : m.h);
            if (free > 0.f && growSum > 0.f) main += free * cs.grow / growSum;
            // a scroll container never shrinks its children: the overflow is what it scrolls
            else if (free < 0.f && shrinkSum > 0.f && s.overflow != Overflow::Scroll) main += free * cs.shrink * main / shrinkSum;
            main = row ? clampSize(main, cs.minWidth, cs.maxWidth, mainAvail)
                       : clampSize(main, cs.minHeight, cs.maxHeight, mainAvail);
            const f32 mBefore = row ? cs.margin.l : cs.margin.t, mAfter = row ? cs.margin.r : cs.margin.b;
            const f32 cmB = row ? cs.margin.t : cs.margin.l, cmA = row ? cs.margin.b : cs.margin.r;
            const Align al = cs.alignSelf != Align::Auto ? cs.alignSelf : s.alignItems;
            const bool crossFixed = row ? fixedLen(cs.height, crossAvail) : fixedLen(cs.width, crossAvail);
            const f32 crossRoom = crossAvail - cmB - cmA;
            f32 cross = row ? m.h : m.w;
            if (al == Align::Stretch && !crossFixed) cross = std::max(0.f, crossRoom);
            f32 co = cmB;
            if (al == Align::Center) co += (crossRoom - cross) * 0.5f;
            else if (al == Align::End) co += crossRoom - cross;
            pos += mBefore;
            const Rect r = row ? Rect{c.x + pos, c.y + co, main, cross} : Rect{c.x + co, c.y + pos, cross, main};
            place(k, {std::round(r.x), std::round(r.y), std::round(r.w), std::round(r.h)});
            pos += main + mAfter + between;
        }
        break;
    }
    case LayoutKind::Grid: {
        const u32 cols = std::max<u32>(1, s.gridColumnCount);
        std::array<f32, kMaxGridTracks> w{}, x{};
        f32 fixedSum = s.gap * f32(cols - 1), frSum = 0.f;
        for (u32 k = 0; k < cols; ++k) {
            const Length t = s.gridColumnCount ? s.gridColumns[k] : Length::fr(1.f);
            if (t.unit == Unit::Px || t.unit == Unit::Percent) { w[k] = t.resolve(c.w, 0.f); fixedSum += w[k]; }
            else frSum += t.unit == Unit::Fr ? t.value : 1.f;
        }
        const f32 frUnit = frSum > 0.f ? std::max(0.f, c.w - fixedSum) / frSum : 0.f;
        for (u32 k = 0; k < cols; ++k) {
            const Length t = s.gridColumnCount ? s.gridColumns[k] : Length::fr(1.f);
            if (!(t.unit == Unit::Px || t.unit == Unit::Percent)) w[k] = frUnit * (t.unit == Unit::Fr ? t.value : 1.f);
            x[k] = k ? x[k - 1] + w[k - 1] + s.gap : 0.f;
        }
        // auto placement row-major; explicit column / row (1-based) honoured, spans within the column count
        u32 col = 0, rowIdx = 0;
        f32 y = 0.f, rowH = 0.f;
        u32 curRow = 0;
        for (u32 k = n.first; k != kNone; k = m_nodes[k].next) {
            if (hidden(k)) continue;
            const ComputedStyle& cs = m_styles.style(m_nodes[k].style);
            const u32 span = std::clamp<u32>(cs.gridColumnSpan, 1, cols);
            if (cs.gridColumn) col = std::min<u32>(cs.gridColumn - 1, cols - 1);
            if (cs.gridRow && cs.gridRow - 1u > rowIdx) { rowIdx = cs.gridRow - 1u; col = cs.gridColumn ? col : 0; }
            if (col + span > cols) { col = 0; ++rowIdx; }
            if (rowIdx != curRow) { y += rowH + s.gap; rowH = 0.f; curRow = rowIdx; }
            const f32 cw = x[col + span - 1] + w[col + span - 1] - x[col];
            const Size m = measure(k, {cw - cs.margin.h(), kInf});
            const f32 h = m.h;
            place(k, {std::round(c.x + x[col] + cs.margin.l), std::round(c.y + y + cs.margin.t), std::round(cw - cs.margin.h()), std::round(h)});
            rowH = std::max(rowH, h + cs.margin.v());
            col += span;
            if (col >= cols) { col = 0; ++rowIdx; }
        }
        break;
    }
    case LayoutKind::Stack:
        for (u32 k = n.first; k != kNone; k = m_nodes[k].next) {
            if (hidden(k)) continue;
            const ComputedStyle& cs = m_styles.style(m_nodes[k].style);
            const Size m = measure(k, {c.w - cs.margin.h(), c.h - cs.margin.v()});
            const f32 w = fixedLen(cs.width, c.w) ? m.w : c.w - cs.margin.h();
            const f32 h = fixedLen(cs.height, c.h) ? m.h : c.h - cs.margin.v();
            place(k, {std::round(c.x + cs.margin.l + (c.w - cs.margin.h() - w) * 0.5f),
                      std::round(c.y + cs.margin.t + (c.h - cs.margin.v() - h) * 0.5f), w, h});
        }
        break;
    case LayoutKind::Anchor:
    case LayoutKind::Absolute:
        for (u32 k = n.first; k != kNone; k = m_nodes[k].next) {
            if (hidden(k)) continue;
            const ComputedStyle& cs = m_styles.style(m_nodes[k].style);
            const Size m = measure(k, {c.w, c.h});
            const bool hasL = !cs.left.isAuto(), hasR = !cs.right.isAuto(), hasT = !cs.top.isAuto(), hasB = !cs.bottom.isAuto();
            const f32 l = cs.left.resolve(c.w, 0.f), r = cs.right.resolve(c.w, 0.f);
            const f32 t = cs.top.resolve(c.h, 0.f), b = cs.bottom.resolve(c.h, 0.f);
            f32 w = m.w, h = m.h, px = l, py = t;
            if (s.layout == LayoutKind::Anchor) {
                if (hasL && hasR && !fixedLen(cs.width, c.w)) w = std::max(0.f, c.w - l - r);
                if (hasT && hasB && !fixedLen(cs.height, c.h)) h = std::max(0.f, c.h - t - b);
                if (cs.centerX) px = (c.w - w) * 0.5f + l - r;
                else if (!hasL && hasR) px = c.w - r - w;
                if (cs.centerY) py = (c.h - h) * 0.5f + t - b;
                else if (!hasT && hasB) py = c.h - b - h;
            }
            if (s.layout == LayoutKind::Anchor && hasClass({k, m_nodes[k].gen}, "clamp")) { // keep popups on screen
                const Rect a = popupArea();
                px = std::max(a.x - c.x, std::min(px, a.x + a.w - c.x - w - cs.margin.h()));
                py = std::max(a.y - c.y, std::min(py, a.y + a.h - c.y - h - cs.margin.v()));
            }
            place(k, {std::round(c.x + px + cs.margin.l), std::round(c.y + py + cs.margin.t), w, h});
        }
        break;
    case LayoutKind::Flow: {
        f32 x = 0.f, y = 0.f, lineH = 0.f;
        for (u32 k = n.first; k != kNone; k = m_nodes[k].next) {
            if (hidden(k)) continue;
            const ComputedStyle& cs = m_styles.style(m_nodes[k].style);
            const Size m = measure(k, {c.w, kInf});
            const f32 mw = m.w + cs.margin.h();
            if (x > 0.f && x + mw > c.w) { y += lineH + s.gap; x = 0.f; lineH = 0.f; }
            place(k, {std::round(c.x + x + cs.margin.l), std::round(c.y + y + cs.margin.t), m.w, m.h});
            x += mw + s.gap;
            lineH = std::max(lineH, m.h + cs.margin.v());
        }
        break;
    }
    }
    n.extent = extent;
    if (s.overflow == Overflow::Scroll) {
        const Vec2 mx{std::max(0.f, extent.w - c.w), std::max(0.f, extent.h - c.h)};
        if (n.scroll.x > mx.x || n.scroll.y > mx.y) {
            n.scroll = {std::min(n.scroll.x, mx.x), std::min(n.scroll.y, mx.y)};
            n.dirty |= kDirtyLayout;
            arrangeChildren(i);
            n.dirty &= u8(~kDirtyLayout);
        }
    }
}

// ---- Paint ---------------------------------------------------------------------------------------------------------
void UiContext::paintRec(u32 i, f32 opacity, DrawList& dl) {
    Node& n = m_nodes[i];
    const ComputedStyle& s = m_styles.style(n.style);
    if (n.dirty & kDirtyPaint) {
        m_damage = m_damage.unite(n.rect);
        n.dirty &= u8(~kDirtyPaint);
    }
    if (!s.visible) return;
    opacity *= s.opacity * n.animOpacity;
    if (opacity <= 0.001f || n.rect.empty()) return;
    ++m_stats.painted;
    const NodeHandle h{i, n.gen};
    auto tint = [&](Color c) { return c.withAlpha(c.a * opacity); };
    const f32 rad = s.radius[0];
    if (s.shadowColor.a > 0.f && (s.shadowBlur > 0.f || s.shadowX != 0.f || s.shadowY != 0.f))
        dl.shadow({n.rect.x + s.shadowX, n.rect.y + s.shadowY, n.rect.w, n.rect.h}, tint(s.shadowColor), s.shadowBlur, rad);
    if (s.backdropBlur > 0.f) dl.backdrop(n.rect, s.backdropBlur, s.radius);
    if (s.gradient != GradientDir::None) {
        dl.gradientRect(n.rect, tint(s.background), tint(s.background2), s.gradient == GradientDir::Vertical, s.radius);
        if (s.borderWidth > 0.f && s.borderColor.a > 0.f) dl.roundedRect(n.rect, {}, s.radius, tint(s.borderColor), s.borderWidth);
    } else if (s.background.a > 0.f || (s.borderWidth > 0.f && s.borderColor.a > 0.f)) {
        dl.roundedRect(n.rect, tint(s.background), s.radius, tint(s.borderColor), s.borderWidth);
    }
    for (std::size_t k = dl.size() - std::min<std::size_t>(dl.size(), 3); k < dl.size(); ++k)
        if (dl.cmds()[k].node == 0xFFFFFFFFu) dl.cmds()[k].node = i;
    const WidgetClass& wc = m_registry.get(n.cls);
    const Rect content = contentRect(h);
    if (wc.paint) {
        const PaintContext pc{*this, h, n.rect, content, s, n.state, opacity};
        wc.paint(pc, dl);
    } else if (!m_cold[i]->text.empty()) {
        const auto& tl = m_cold[i]->layout;
        const f32 y = content.y + std::floor(std::max(0.f, content.h - tl.height) * 0.5f);
        paintTextLayout(dl, tl, {content.x, y}, tint(s.color), s.fontSize, i);
    }
    if (n.first == kNone) return;
    const bool clip = s.overflow != Overflow::Visible;
    if (clip) dl.pushClip(n.rect, rad);
    for (u32 k = n.first; k != kNone; k = m_nodes[k].next) {
        if (clip && m_nodes[k].rect.intersect(n.rect).empty()) { // culled; still consume dirty flags
            if (m_nodes[k].dirty & kDirtyPaint) { m_damage = m_damage.unite(m_nodes[k].rect.intersect(n.rect)); m_nodes[k].dirty &= u8(~kDirtyPaint); }
            continue;
        }
        if (m_osPopups && &dl == &m_draw && hasClass({k, m_nodes[k].gen}, "clamp")) { // own OS window
            m_popupPending.push_back(k);
            continue;
        }
        paintRec(k, opacity, dl);
    }
    if (clip) dl.popClip();
    if (s.overflow == Overflow::Scroll)
        for (u8 ax = 0; ax < 2; ++ax) {
            Rect tr, th;
            if (!scrollBarGeom(i, ax, tr, th)) continue;
            const bool hot = m_scrollDrag.node.index == i && m_scrollDrag.axis == ax;
            dl.rect(tr, tint(Color::rgb(0x000000, 0.12f)), kScrollBarSize * 0.5f);
            dl.rect(th, tint(Color::rgb(hot ? 0xB4B4C0 : 0x8A8A96, 0.85f)), kScrollBarSize * 0.5f);
        }
}

bool UiContext::scrollBarGeom(u32 i, u8 axis, Rect& track, Rect& thumb) const {
    const Node& n = m_nodes[i];
    const Rect c = contentRect({i, n.gen});
    const f32 view = axis ? c.h : c.w, ext = axis ? n.extent.h : n.extent.w;
    if (ext <= view + 0.5f || view <= 0.f) return false;
    const f32 b = kScrollBarSize;
    track = axis ? Rect{n.rect.x + n.rect.w - b - 1.f, n.rect.y + 1.f, b, n.rect.h - 2.f}
                 : Rect{n.rect.x + 1.f, n.rect.y + n.rect.h - b - 1.f, n.rect.w - 2.f - b, b};
    const f32 len = axis ? track.h : track.w;
    const f32 tl = std::max(16.f, len * view / ext);
    const f32 at = (len - tl) * std::clamp((axis ? n.scroll.y : n.scroll.x) / (ext - view), 0.f, 1.f);
    thumb = axis ? Rect{track.x, track.y + at, b, tl} : Rect{track.x + at, track.y, tl, b};
    return true;
}

void UiContext::paintPass() {
    m_stats.repainted = false;
    if (!m_anyPaintDirty) return;
    m_draw.clear();
    m_popupPending.clear();
    paintRec(m_root.index, 1.f, m_draw);
    std::vector<PopupSurface> next;
    for (u32 k : m_popupPending) {
        PopupSurface ps;
        ps.node = {k, m_nodes[k].gen};
        ps.rect = m_nodes[k].rect;
        const Rect saved = m_damage;
        paintRec(k, 1.f, ps.draw);
        m_damage = saved; // a popup window repaints itself; the main window is not damaged by it
        std::erase_if(ps.draw.cmds(), [&](const DrawCmd& c) { return c.op == DrawOp::Shadow && c.node == k; }); // OS shadow
        if (ps.draw.size() == 0 || ps.rect.empty()) continue;
        ps.draw.translate({-ps.rect.x, -ps.rect.y});
        ps.revision = ++m_popupRevision;
        for (const PopupSurface& old : m_popupSurfaces)
            if (old.node == ps.node && old.rect.x == ps.rect.x && old.rect.y == ps.rect.y && old.rect.w == ps.rect.w &&
                old.rect.h == ps.rect.h && old.draw.hash() == ps.draw.hash())
                ps.revision = old.revision;
        next.push_back(std::move(ps));
    }
    if (next.size() != m_popupSurfaces.size()) m_damage = m_damage.unite({0.f, 0.f, m_viewport.w, m_viewport.h});
    m_popupSurfaces = std::move(next);
    m_anyPaintDirty = false;
    m_stats.repainted = true;
    m_stats.drawCmds = u32(m_draw.size());
}

void UiContext::setOsPopups(bool on, Rect screen) {
    if (on == m_osPopups && screen.x == m_popupScreen.x && screen.y == m_popupScreen.y && screen.w == m_popupScreen.w &&
        screen.h == m_popupScreen.h)
        return;
    m_osPopups = on;
    m_popupScreen = screen;
    if (!on) m_popupSurfaces.clear();
    markAllDirty();
}
Rect UiContext::popupArea() const {
    if (m_osPopups && !m_popupScreen.empty()) return m_popupScreen;
    return {0.f, 0.f, m_viewport.w, m_viewport.h};
}

// ---- Frame ---------------------------------------------------------------------------------------------------------
void UiContext::update(f32 dt) {
    ++m_frame;
    m_stats = {};
    m_damage = {};
    drainCommands();
    if (dt > 0.f) {
        for (u32 t = 0; t < m_timers.size(); ++t) {
            if (!m_timers[t].active) continue;
            m_timers[t].left -= dt;
            int guard = 0;
            while (m_timers[t].active && m_timers[t].left <= 0.f && guard++ < 8) {
                const auto fn = m_timers[t].fn;
                if (m_timers[t].repeat && m_timers[t].interval > 0.f) m_timers[t].left += m_timers[t].interval;
                else m_timers[t].active = false;
                if (fn) fn(*this);
            }
        }
    }
    if (m_animCount) {
        for (auto& a : m_anims) {
            if (!a.active) continue;
            Node* n = node(a.node);
            if (!n) { a.active = false; --m_animCount; continue; }
            a.t += dt;
            f32 k = a.duration > 0.f ? std::clamp(a.t / a.duration, 0.f, 1.f) : 1.f;
            switch (a.ease) {
            case Ease::Linear: break;
            case Ease::InOutCubic: k = k < 0.5f ? 4.f * k * k * k : 1.f - std::pow(-2.f * k + 2.f, 3.f) * 0.5f; break;
            case Ease::OutBack: { const f32 c1 = 1.70158f, c3 = c1 + 1.f; k = 1.f + c3 * std::pow(k - 1.f, 3.f) + c1 * std::pow(k - 1.f, 2.f); break; }
            }
            const f32 v = a.from + (a.to - a.from) * k;
            switch (a.prop) {
            case AnimProp::Opacity: n->animOpacity = std::clamp(v, 0.f, 1.f); markPaint(a.node.index); break;
            case AnimProp::OffsetX: n->offset.x = v; if (n->parent != kNone) markArrange(n->parent); n->dirty |= kDirtyLayout; break;
            case AnimProp::OffsetY: n->offset.y = v; if (n->parent != kNone) markArrange(n->parent); n->dirty |= kDirtyLayout; break;
            case AnimProp::Value: setValue(a.node, v, true); break;
            }
            if (a.t >= a.duration) { a.active = false; --m_animCount; }
        }
    }
    if (m_anyStyleDirty || m_styles.sheetVersion() != m_seenSheetVersion) stylePass();
    if (m_anyLayoutDirty) {
        Node& r = m_nodes[m_root.index];
        if (r.measureKey != Size{m_viewport.w, m_viewport.h}) measure(m_root.index, m_viewport);
        arrange(m_root.index, {0.f, 0.f, m_viewport.w, m_viewport.h});
        m_anyLayoutDirty = false;
    }
    paintPass();
}

bool UiContext::needsUpdate() const {
    if (m_anyStyleDirty || m_anyLayoutDirty || m_anyPaintDirty || m_animCount) return true;
    if (m_styles.sheetVersion() != m_seenSheetVersion) return true;
    for (const auto& t : m_timers)
        if (t.active) return true;
    return m_head.load(std::memory_order_acquire) != m_tail;
}

f32 UiContext::nextWakeSeconds() const {
    if (m_anyStyleDirty || m_anyLayoutDirty || m_anyPaintDirty || m_animCount) return 0.f;
    if (m_styles.sheetVersion() != m_seenSheetVersion) return 0.f;
    if (m_head.load(std::memory_order_acquire) != m_tail) return 0.f;
    f32 t = std::numeric_limits<f32>::infinity();
    for (const auto& tm : m_timers)
        if (tm.active) t = std::min(t, std::max(0.f, tm.left));
    return t;
}

// ---- Hit testing and dispatch --------------------------------------------------------------------------------------
NodeHandle UiContext::hitRec(u32 i, Vec2 p) const {
    const Node& n = m_nodes[i];
    const ComputedStyle& s = m_styles.style(n.style);
    if (!s.visible || n.animOpacity <= 0.f) return {};
    const bool inside = n.rect.contains(p);
    if (!inside && s.overflow != Overflow::Visible) return {};
    for (u32 k = n.last; k != kNone; k = m_nodes[k].prev) {
        const NodeHandle h = hitRec(k, p);
        if (h.valid()) return h;
    }
    if (inside && s.pointerEvents) return {i, n.gen};
    return {};
}
NodeHandle UiContext::hitTest(Vec2 p) const { return hitRec(m_root.index, p); }

bool UiContext::dispatch(u32 target, const Event& e) {
    m_path.clear();
    for (u32 a = target; a != kNone; a = m_nodes[a].parent) m_path.push_back(a);
    const NodeHandle th{target, m_nodes[target].gen};
    auto call = [&](u32 k, Phase ph) {
        if (!m_nodes[k].alive) return false;
        const WidgetClass& wc = m_registry.get(m_nodes[k].cls);
        if (!wc.onEvent) return false;
        EventContext ec{*this, {k, m_nodes[k].gen}, th, ph};
        return wc.onEvent(ec, e);
    };
    for (std::size_t k = m_path.size(); k-- > 1;)
        if (call(m_path[k], Phase::Capture)) return true;
    if (call(target, Phase::Target)) return true;
    if (m_nodes[target].alive) emit(th, SignalKind::Event, {.event = &e});
    // path may have been rebuilt by a nested dispatch; recompute the ancestors
    for (u32 a = m_nodes[target].alive ? m_nodes[target].parent : kNone; a != kNone; a = m_nodes[a].parent)
        if (call(a, Phase::Bubble)) return true;
    return false;
}

void UiContext::setHover(u32 i) {
    const u32 old = node(m_hover) ? m_hover.index : kNone;
    if (old == i) return;
    for (u32 a = old; a != kNone; a = m_nodes[a].parent) setState({a, m_nodes[a].gen}, kStateHover, false);
    for (u32 a = i; a != kNone; a = m_nodes[a].parent) setState({a, m_nodes[a].gen}, kStateHover, true);
    m_hover = handleOf(i);
    if (old != kNone) {
        Event e; e.type = EventType::PointerLeave; e.pos = m_pointer;
        dispatch(old, e);
        if (m_nodes[old].alive) emit({old, m_nodes[old].gen}, SignalKind::HoverChanged, {.flag = false});
    }
    if (i != kNone && m_nodes[i].alive) {
        Event e; e.type = EventType::PointerEnter; e.pos = m_pointer;
        dispatch(i, e);
        emit({i, m_nodes[i].gen}, SignalKind::HoverChanged, {.flag = true});
    }
}

u32 UiContext::interactiveAncestor(u32 i) const {
    for (u32 a = i; a != kNone; a = m_nodes[a].parent)
        if (m_registry.get(m_nodes[a].cls).interactive) return a;
    return kNone;
}

bool UiContext::visibleChain(u32 i) const {
    for (u32 a = i; a != kNone; a = m_nodes[a].parent)
        if (!m_styles.style(m_nodes[a].style).visible) return false;
    return true;
}

bool UiContext::pointerMove(Vec2 p) {
    m_pointer = p;
    if (const Node* sn = node(m_scrollDrag.node)) {
        Rect tr, th;
        const u32 si = m_scrollDrag.node.index;
        if (scrollBarGeom(si, m_scrollDrag.axis, tr, th)) {
            const u8 ax = m_scrollDrag.axis;
            const Rect c = contentRect(m_scrollDrag.node);
            const f32 len = ax ? tr.h - th.h : tr.w - th.w, ext = ax ? sn->extent.h - c.h : sn->extent.w - c.w;
            const f32 f = std::clamp(((ax ? p.y - tr.y : p.x - tr.x) - m_scrollDrag.grab) / std::max(1.f, len), 0.f, 1.f);
            scrollTo(m_scrollDrag.node, ax ? Vec2{sn->scroll.x, f * ext} : Vec2{f * ext, sn->scroll.y});
        }
        return true;
    }
    NodeHandle h = hitTest(p);
    if (node(m_modal) && h.valid() && !inScope(h.index, m_modal.index)) h = {};
    setHover(h.valid() ? h.index : kNone);
    Event e; e.type = EventType::PointerMove; e.pos = p; e.mods = m_mods;
    if (node(m_pressed)) return dispatch(m_pressed.index, e) || true;
    if (h.valid() && h != m_root) { dispatch(h.index, e); return true; }
    return node(m_modal) != nullptr;
}

bool UiContext::pointerDown(Vec2 p, MouseButton b) {
    m_pointer = p;
    const NodeHandle h = hitTest(p);
    if (node(m_modal) && (!h.valid() || !inScope(h.index, m_modal.index))) return true; // blocked by modal
    if (b == MouseButton::Left)
        for (u32 a = h.valid() ? h.index : kNone; a != kNone; a = m_nodes[a].parent) {
            if (m_styles.style(m_nodes[a].style).overflow != Overflow::Scroll) continue;
            for (u8 ax = 2; ax-- > 0;) {
                Rect tr, th;
                if (!scrollBarGeom(a, ax, tr, th) || !tr.contains(p)) continue;
                const f32 pp = ax ? p.y : p.x, t0 = ax ? th.y : th.x, tl = ax ? th.h : th.w;
                m_scrollDrag = {{a, m_nodes[a].gen}, ax, (pp >= t0 && pp < t0 + tl) ? pp - t0 : tl * 0.5f};
                pointerMove(p); // jump-to / no-op for a thumb grab
                markPaint(a);
                return true;
            }
        }
    if (!h.valid() || h == m_root) {
        if (b == MouseButton::Left) setFocus({}, FocusReason::Pointer);
        return false;
    }
    setHover(h.index);
    u32 target = interactiveAncestor(h.index);
    if (target == kNone) target = h.index;
    if (m_nodes[target].state & kStateDisabled) return true;
    if (b == MouseButton::Left) {
        m_pressed = {target, m_nodes[target].gen};
        setState(m_pressed, kStatePressed, true);
        u32 f = target;
        while (f != kNone && !focusableNode(f)) f = m_nodes[f].parent;
        setFocus(handleOf(f), FocusReason::Pointer);
    }
    Event e; e.type = EventType::PointerDown; e.pos = p; e.button = b; e.mods = m_mods;
    dispatch(target, e);
    return true;
}

bool UiContext::pointerUp(Vec2 p, MouseButton b) {
    m_pointer = p;
    if (m_scrollDrag.node.valid() && b == MouseButton::Left) {
        if (node(m_scrollDrag.node)) markPaint(m_scrollDrag.node.index);
        m_scrollDrag = {};
        return true;
    }
    const NodeHandle pr = m_pressed;
    if (!node(pr) || b != MouseButton::Left) {
        const NodeHandle h = hitTest(p);
        if (h.valid() && h != m_root) {
            Event e; e.type = EventType::PointerUp; e.pos = p; e.button = b; e.mods = m_mods;
            dispatch(h.index, e);
            return true;
        }
        return node(m_modal) != nullptr;
    }
    m_pressed = {};
    setState(pr, kStatePressed, false);
    Event e; e.type = EventType::PointerUp; e.pos = p; e.button = b; e.mods = m_mods;
    dispatch(pr.index, e);
    const NodeHandle h = hitTest(p);
    bool inside = false;
    for (u32 a = h.valid() ? h.index : kNone; a != kNone; a = m_nodes[a].parent)
        if (a == pr.index) { inside = true; break; }
    if (inside && node(pr) && !(m_nodes[pr.index].state & kStateDisabled)) {
        Event c; c.type = EventType::Click; c.pos = p; c.button = b; c.mods = m_mods;
        if (!dispatch(pr.index, c) && node(pr)) emit(pr, SignalKind::Clicked, {});
    }
    return true;
}

bool UiContext::wheel(Vec2 p, Vec2 notches) {
    const NodeHandle h = hitTest(p);
    if (!h.valid()) return false;
    if (node(m_modal) && !inScope(h.index, m_modal.index)) return true;
    Event e; e.type = EventType::Wheel; e.pos = p; e.wheel = notches; e.mods = m_mods;
    if (dispatch(h.index, e)) return true;
    for (u32 a = h.index; a != kNone; a = m_nodes[a].parent) {
        const Node& n = m_nodes[a];
        if (m_styles.style(n.style).overflow != Overflow::Scroll) continue;
        const Rect c = contentRect({a, n.gen});
        if (n.extent.h <= c.h && n.extent.w <= c.w) continue;
        scrollTo({a, n.gen}, {n.scroll.x - notches.x * 40.f, n.scroll.y - notches.y * 40.f});
        return true;
    }
    return h != m_root;
}

bool UiContext::keyDown(Key k, u8 mods) {
    Event e; e.type = EventType::KeyDown; e.key = k; e.mods = mods;
    if (node(m_focus)) {
        if (dispatch(m_focus.index, e)) return true;
        if ((k == Key::Enter || k == Key::Space) && node(m_focus) && m_registry.get(m_nodes[m_focus.index].cls).interactive &&
            !(m_nodes[m_focus.index].state & kStateDisabled)) {
            m_focusVisible = true;
            const NodeHandle f = m_focus;
            Event c; c.type = EventType::Click;
            if (!dispatch(f.index, c) && node(f)) emit(f, SignalKind::Clicked, {});
            return true;
        }
    }
    if (k == Key::Tab) return focusNext((mods & kModShift) != 0);
    if (m_spatialNav || !node(m_focus)) {
        switch (k) {
        case Key::Left: return navigate(NavDir::Left);
        case Key::Right: return navigate(NavDir::Right);
        case Key::Up: return navigate(NavDir::Up);
        case Key::Down: return navigate(NavDir::Down);
        default: break;
        }
    }
    return node(m_focus) != nullptr && m_registry.get(m_nodes[m_focus.index].cls).focusable;
}

bool UiContext::keyUp(Key k, u8 mods) {
    if (!node(m_focus)) return false;
    Event e; e.type = EventType::KeyUp; e.key = k; e.mods = mods;
    return dispatch(m_focus.index, e);
}

bool UiContext::textInput(std::string_view utf8) {
    if (!node(m_focus)) return false;
    Event e; e.type = EventType::Text; e.text = utf8;
    return dispatch(m_focus.index, e);
}

bool UiContext::textComposition(std::string_view utf8, i32 caret) {
    if (!node(m_focus)) return false;
    m_composition.assign(utf8);
    m_compositionCaret = std::clamp(caret, 0, i32(m_composition.size()));
    Event e; e.type = EventType::Composition; e.text = m_composition; e.caret = m_compositionCaret;
    return dispatch(m_focus.index, e);
}

bool UiContext::setClipboardText(std::string_view utf8) {
    if (m_clipboard.set) return m_clipboard.set(m_clipboard.user, utf8);
    m_localClipboard.assign(utf8);
    return true;
}

bool UiContext::clipboardText(std::string& out) const {
    if (m_clipboard.get) return m_clipboard.get(m_clipboard.user, out);
    out = m_localClipboard;
    return !out.empty();
}

// ---- Focus ---------------------------------------------------------------------------------------------------------
bool UiContext::focusableNode(u32 i) const {
    if (i >= m_nodes.size() || !m_nodes[i].alive) return false;
    const Node& n = m_nodes[i];
    if (!(n.flags & kFlagFocusable) || n.tabIndex < 0 || (n.state & kStateDisabled)) return false;
    for (u32 a = i; a != kNone; a = m_nodes[a].parent)
        if (m_nodes[a].state & kStateDisabled) return false;
    return visibleChain(i);
}
u32 UiContext::scopeOf(u32 i) const {
    if (node(m_modal)) return m_modal.index;
    for (u32 a = m_nodes[i].parent; a != kNone; a = m_nodes[a].parent)
        if (m_nodes[a].flags & kFlagFocusScope) return a;
    return m_root.index;
}
bool UiContext::inScope(u32 i, u32 scope) const {
    for (u32 a = i; a != kNone; a = m_nodes[a].parent)
        if (a == scope) return true;
    return false;
}

void UiContext::collectTab(std::vector<u32>& out) const {
    out.clear();
    const u32 scope = node(m_modal) ? m_modal.index : (node(m_focus) ? scopeOf(m_focus.index) : m_root.index);
    auto& tab = const_cast<std::vector<std::pair<u32, i32>>&>(m_scratchTab);
    tab.clear();
    // pre-order walk without recursion
    u32 i = scope;
    while (i != kNone) {
        if (focusableNode(i) && i != scope) tab.emplace_back(i, m_nodes[i].tabIndex);
        if (m_nodes[i].first != kNone && m_styles.style(m_nodes[i].style).visible) { i = m_nodes[i].first; continue; }
        while (i != kNone && i != scope && m_nodes[i].next == kNone) i = m_nodes[i].parent;
        if (i == kNone || i == scope) break;
        i = m_nodes[i].next;
    }
    std::stable_sort(tab.begin(), tab.end(), [](const auto& a, const auto& b) {
        const i32 ka = a.second > 0 ? a.second : 0x7FFFFFFF, kb = b.second > 0 ? b.second : 0x7FFFFFFF;
        return ka < kb;
    });
    for (const auto& t : tab) out.push_back(t.first);
}

void UiContext::tabOrder(std::vector<NodeHandle>& out) const {
    out.clear();
    auto& tmp = const_cast<std::vector<u32>&>(m_scratchOrder);
    collectTab(tmp);
    for (u32 i : tmp) out.push_back({i, m_nodes[i].gen});
}

bool UiContext::focusNext(bool backward) {
    collectTab(m_scratchOrder);
    if (m_scratchOrder.empty()) return false;
    std::size_t at = m_scratchOrder.size();
    for (std::size_t k = 0; k < m_scratchOrder.size(); ++k)
        if (node(m_focus) && m_scratchOrder[k] == m_focus.index) at = k;
    std::size_t nx;
    const std::size_t sz = m_scratchOrder.size();
    if (at == sz) nx = backward ? sz - 1 : 0;
    else nx = backward ? (at + sz - 1) % sz : (at + 1) % sz;
    setFocus(handleOf(m_scratchOrder[nx]), FocusReason::Keyboard);
    return true;
}

bool UiContext::navigate(NavDir d) {
    collectTab(m_scratchOrder);
    if (m_scratchOrder.empty()) return false;
    if (!node(m_focus)) { setFocus(handleOf(m_scratchOrder[0]), FocusReason::Keyboard); return true; }
    const Rect fr = m_nodes[m_focus.index].rect;
    const Vec2 fc{fr.x + fr.w * 0.5f, fr.y + fr.h * 0.5f};
    u32 best = kNone;
    f32 bestScore = kInf;
    for (u32 i : m_scratchOrder) {
        if (i == m_focus.index) continue;
        const Rect r = m_nodes[i].rect;
        const f32 dx = r.x + r.w * 0.5f - fc.x, dy = r.y + r.h * 0.5f - fc.y;
        f32 along, across;
        switch (d) {
        case NavDir::Left: along = -dx; across = dy; break;
        case NavDir::Right: along = dx; across = dy; break;
        case NavDir::Up: along = -dy; across = dx; break;
        default: along = dy; across = dx; break;
        }
        if (along <= 0.5f) continue;
        const f32 score = along + 2.f * std::abs(across);
        if (score < bestScore) { bestScore = score; best = i; }
    }
    if (best == kNone) return false;
    setFocus(handleOf(best), FocusReason::Keyboard);
    return true;
}

void UiContext::setFocus(NodeHandle h, FocusReason why) {
    if (h.valid() && !node(h)) h = {};
    if (h.valid() && node(m_modal) && !inScope(h.index, m_modal.index)) return;
    const bool vis = why == FocusReason::Keyboard;
    if (h == m_focus) {
        if (node(h) && vis != m_focusVisible) { m_focusVisible = vis; setState(h, kStateFocusVisible, vis); }
        return;
    }
    const NodeHandle old = m_focus;
    m_focus = h;
    m_focusVisible = vis;
    m_composition.clear(); // pre-edit belongs to the old field; the IME is reset by the platform glue
    m_compositionCaret = 0;
    m_caretNode = {};
    if (node(old)) {
        setState(old, kStateFocus | kStateFocusVisible, false);
        Event e; e.type = EventType::FocusOut; e.focusReason = why;
        dispatch(old.index, e);
        if (node(old)) emit(old, SignalKind::FocusChanged, {.flag = false});
    }
    if (node(h)) {
        setState(h, kStateFocus, true);
        setState(h, kStateFocusVisible, vis);
        Event e; e.type = EventType::FocusIn; e.focusReason = why;
        dispatch(h.index, e);
        if (node(h)) emit(h, SignalKind::FocusChanged, {.flag = true});
    }
}

void UiContext::setModal(NodeHandle scope) {
    m_modal = node(scope) ? scope : NodeHandle{};
    if (node(m_modal) && !(node(m_focus) && inScope(m_focus.index, m_modal.index))) {
        setFocus({}, FocusReason::Programmatic);
        focusNext(false);
    }
}

// ---- Signals -------------------------------------------------------------------------------------------------------
ConnectionHandle UiContext::connect(NodeHandle h, SignalKind kind, Slot slot) {
    if (!node(h) || !slot) return {};
    u32 i;
    if (!m_freeConnections.empty()) { i = m_freeConnections.back(); m_freeConnections.pop_back(); }
    else { i = u32(m_connections.size()); m_connections.emplace_back(); }
    Connection& c = m_connections[i];
    c.node = h.index;
    c.nodeGen = h.generation;
    c.kind = kind;
    c.slot = slot;
    ++c.gen;
    return {i, c.gen};
}
void UiContext::disconnect(ConnectionHandle h) {
    if (h.index >= m_connections.size() || m_connections[h.index].gen != h.generation) return;
    Connection& c = m_connections[h.index];
    c.node = kNone;
    c.slot = Slot{};
    ++c.gen;
    m_freeConnections.push_back(h.index);
}
void UiContext::emit(NodeHandle h, SignalKind kind, const SignalArgs& args) {
    const std::size_t count = m_connections.size();
    for (std::size_t i = 0; i < count; ++i) {
        const Connection& c = m_connections[i];
        if (c.node != h.index || c.nodeGen != h.generation || c.kind != kind || !c.slot) continue;
        const Slot s = c.slot;
        s(*this, h, args);
        if (!node(h)) return;
    }
}

// ---- Timers / animation --------------------------------------------------------------------------------------------
TimerHandle UiContext::startTimer(f32 seconds, bool repeat, InlineFn<void(UiContext&)> fn) {
    u32 i = 0;
    while (i < m_timers.size() && m_timers[i].active) ++i;
    if (i == m_timers.size()) m_timers.emplace_back();
    Timer& t = m_timers[i];
    t.interval = t.left = seconds;
    t.repeat = repeat;
    t.active = true;
    t.fn = fn;
    ++t.gen;
    return {i, t.gen};
}
void UiContext::cancelTimer(TimerHandle h) {
    if (h.index < m_timers.size() && m_timers[h.index].gen == h.generation) m_timers[h.index].active = false;
}
void UiContext::animate(NodeHandle h, AnimProp p, f32 to, f32 seconds, Ease e) {
    Node* n = node(h);
    if (!n) return;
    f32 from = 0.f;
    switch (p) {
    case AnimProp::Opacity: from = n->animOpacity; break;
    case AnimProp::OffsetX: from = n->offset.x; break;
    case AnimProp::OffsetY: from = n->offset.y; break;
    case AnimProp::Value: from = n->value; break;
    }
    Anim* slot = nullptr;
    for (auto& a : m_anims)
        if (a.active && a.node == h && a.prop == p) { slot = &a; break; }
    if (!slot) {
        for (auto& a : m_anims)
            if (!a.active) { slot = &a; break; }
        if (!slot) slot = &m_anims.emplace_back();
        slot->active = true;
        ++m_animCount;
    }
    *slot = Anim{h, p, e, from, to, 0.f, std::max(0.f, seconds), true};
}

// ---- Cross-thread commands (Vyukov bounded queue; multi-producer, single consumer) -------------------------------
bool UiContext::post(CommandFn fn, NodeHandle n, u64 a, u64 b) {
    u64 pos = m_head.load(std::memory_order_relaxed);
    for (;;) {
        Command& c = m_queue[pos & (kQueueSize - 1)];
        const u64 seq = c.seq.load(std::memory_order_acquire);
        const i64 dif = i64(seq) - i64(pos);
        if (dif == 0) {
            if (m_head.compare_exchange_weak(pos, pos + 1, std::memory_order_relaxed)) {
                c.fn = fn; c.node = n; c.a = a; c.b = b;
                c.seq.store(pos + 1, std::memory_order_release);
                return true;
            }
        } else if (dif < 0) {
            return false;
        } else {
            pos = m_head.load(std::memory_order_relaxed);
        }
    }
}
void UiContext::drainCommands() {
    for (;;) {
        Command& c = m_queue[m_tail & (kQueueSize - 1)];
        if (c.seq.load(std::memory_order_acquire) != m_tail + 1) return;
        const CommandFn fn = c.fn;
        const NodeHandle n = c.node;
        const u64 a = c.a, b = c.b;
        c.seq.store(m_tail + kQueueSize, std::memory_order_release);
        ++m_tail;
        if (fn) fn(*this, n, a, b);
    }
}

// ---- Style sheets --------------------------------------------------------------------------------------------------
std::expected<void, UiError> UiContext::addStyleSheet(std::string_view css, std::vector<StyleDiagnostic>* diags) {
    auto sheet = StyleSheet::parse(css, diags);
    if (!sheet) {
        if (diags) diags->push_back(sheet.error());
        return std::unexpected(UiError::StyleParse);
    }
    m_styles.addSheet(std::move(*sheet));
    m_anyStyleDirty = true;
    return {};
}
void UiContext::addStyleSheet(StyleSheet sheet) {
    m_styles.addSheet(std::move(sheet));
    m_anyStyleDirty = true;
}
void UiContext::clearStyleSheets() {
    m_styles.clearSheets();
    m_anyStyleDirty = true;
}

// ---- Dumps ---------------------------------------------------------------------------------------------------------
namespace {
void appendf(std::string& out, const char* fmt, auto... a) {
    char buf[256];
    const int n = std::snprintf(buf, sizeof buf, fmt, a...);
    if (n > 0) out.append(buf, std::size_t(std::min(n, int(sizeof buf) - 1)));
}
} // namespace

void UiContext::dumpLayout(std::string& out) const {
    auto rec = [&](auto&& self, u32 i, int depth) -> void {
        const Node& n = m_nodes[i];
        out.append(std::size_t(depth) * 2, ' ');
        out += m_registry.get(n.cls).name;
        if (!m_cold[i]->id.empty()) { out += '#'; out += m_cold[i]->id; }
        appendf(out, " %g %g %g %g\n", double(n.rect.x), double(n.rect.y), double(n.rect.w), double(n.rect.h));
        for (u32 k = n.first; k != kNone; k = m_nodes[k].next) self(self, k, depth + 1);
    };
    rec(rec, m_root.index, 0);
}

void UiContext::dumpA11y(std::string& out) const {
    auto rec = [&](auto&& self, u32 i, int depth) -> void {
        const Node& n = m_nodes[i];
        if (!m_styles.style(n.style).visible) return;
        int d = depth;
        if (n.role != A11yRole::None) {
            out.append(std::size_t(depth) * 2, ' ');
            out += a11yRoleName(n.role);
            const std::string& nm = m_cold[i]->a11yName.empty() ? m_cold[i]->text : m_cold[i]->a11yName;
            if (!nm.empty()) { out += " \""; out += nm; out += '"'; }
            if (n.flags & kFlagFocusable) out += " focusable";
            if (m_focus.index == i && m_focus.generation == n.gen) out += " focused";
            if (n.state & kStateChecked) out += " checked";
            if (n.state & kStateDisabled) out += " disabled";
            switch (n.role) {
            case A11yRole::Slider: case A11yRole::ProgressBar: case A11yRole::SpinBox: case A11yRole::Dial: case A11yRole::ScrollBar:
                appendf(out, " value=%g range=[%g,%g]", double(n.value), double(n.lo), double(n.hi));
                break;
            default: break;
            }
            out += '\n';
            d = depth + 1;
        }
        for (u32 k = n.first; k != kNone; k = m_nodes[k].next) self(self, k, d);
    };
    rec(rec, m_root.index, 0);
}

void UiContext::snapshotA11y(std::vector<A11ySnapshotNode>& out) const {
    out.clear();
    auto rec = [&](auto&& self, u32 i, u32 parent) -> void {
        const Node& n = m_nodes[i];
        if (!m_styles.style(n.style).visible) return;
        u32 p = parent;
        if (n.role != A11yRole::None || i == m_root.index) {
            A11ySnapshotNode s;
            s.node = {i, n.gen};
            s.parent = parent;
            s.role = n.role == A11yRole::None ? A11yRole::Window : n.role;
            s.name = m_cold[i]->a11yName.empty() ? m_cold[i]->text : m_cold[i]->a11yName;
            s.bounds = n.rect;
            s.focusable = (n.flags & kFlagFocusable) != 0;
            s.focused = m_focus.index == i && m_focus.generation == n.gen;
            s.checked = (n.state & kStateChecked) != 0;
            s.disabled = (n.state & kStateDisabled) != 0;
            s.value = n.value;
            s.lo = n.lo;
            s.hi = n.hi;
            p = u32(out.size());
            out.push_back(std::move(s));
        }
        for (u32 k = n.first; k != kNone; k = m_nodes[k].next) self(self, k, p);
    };
    rec(rec, m_root.index, ~0u);
}

} // namespace fuse::ui
