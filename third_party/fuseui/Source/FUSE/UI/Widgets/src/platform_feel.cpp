#include "fuse/ui/widgets/platform_feel.hpp"
#include "fuse/ui/widgets/widgets.hpp"

#include <algorithm>
#include <cmath>
#include <memory>

namespace fuse::ui::widgets {

namespace {
HapticsFn g_haptics = nullptr;
void* g_hapticsUser = nullptr;
} // namespace

void setHapticsHandler(HapticsFn fn, void* user) {
    g_haptics = fn;
    g_hapticsUser = user;
}
void haptic(HapticKind kind) {
    if (g_haptics) g_haptics(g_hapticsUser, kind);
}

ScrollProfile ScrollProfile::desktop() {
    ScrollProfile p;
    p.decay = 9.f;
    p.overscroll = false;
    p.maxVelocity = 5000.f;
    return p;
}
ScrollProfile ScrollProfile::ios() { return ScrollProfile{}; }
ScrollProfile ScrollProfile::android() {
    ScrollProfile p;
    p.decay = 3.2f;
    p.overscroll = false;
    p.maxVelocity = 9000.f;
    return p;
}

f32 KineticScroller::rubber(f32 raw) const {
    if (!m_p.overscroll) return std::clamp(raw, m_lo, m_hi);
    const f32 d = std::max(1.f, (m_hi - m_lo) * 0.25f + 100.f); // dimension the band saturates at
    auto band = [&](f32 x) { return (1.f - 1.f / (x * m_p.rubberBand / d + 1.f)) * d; };
    if (raw < m_lo) return m_lo - band(m_lo - raw);
    if (raw > m_hi) return m_hi + band(raw - m_hi);
    return raw;
}

void KineticScroller::press(f32 pos, f32 t) {
    m_pressed = true;
    m_v = 0.f;
    m_pressPos = pos;
    m_pressOff = m_off;
    m_samples.clear();
    m_samples.push_back({t, pos});
}

void KineticScroller::drag(f32 pos, f32 t) {
    if (!m_pressed) return;
    const bool wasInside = overscroll() == 0.f;
    m_off = rubber(m_pressOff + (m_pressPos - pos));
    if (wasInside && overscroll() != 0.f) haptic(HapticKind::Edge);
    m_samples.push_back({t, pos});
    while (m_samples.size() > 2 && t - m_samples.front().t > m_p.sampleWindow) m_samples.erase(m_samples.begin());
}

void KineticScroller::release(f32 t) {
    if (!m_pressed) return;
    m_pressed = false;
    m_v = 0.f;
    // velocity from the samples inside the window (finger moving up = content scrolls down = +offset)
    const Sample* first = nullptr;
    for (const Sample& s : m_samples)
        if (t - s.t <= m_p.sampleWindow + 1e-4f) { first = &s; break; }
    if (first && &m_samples.back() != first) {
        const Sample& last = m_samples.back();
        const f32 dt = last.t - first->t;
        if (dt > 1e-4f) m_v = std::clamp(-(last.pos - first->pos) / dt, -m_p.maxVelocity, m_p.maxVelocity);
    }
    if (std::abs(m_v) < m_p.minVelocity) m_v = 0.f;
}

bool KineticScroller::step(f32 dt) {
    if (m_pressed) return true;
    const f32 over = overscroll();
    if (over != 0.f) {
        if (!m_p.overscroll) { m_off = clamped(); m_v = 0.f; return false; }
        // moving further out: strong deceleration; otherwise critically damped spring back to the edge
        const f32 k = m_p.springStiffness, c = 2.f * std::sqrt(k);
        const f32 a = -k * over - c * m_v;
        m_v += a * dt;
        m_off += m_v * dt;
        if (std::abs(overscroll()) < 0.25f && std::abs(m_v) < 20.f) { m_off = clamped(); m_v = 0.f; return false; }
        if ((over > 0.f && overscroll() < 0.f) || (over < 0.f && overscroll() > 0.f)) { m_off = clamped(); m_v = 0.f; return false; }
        return true;
    }
    if (m_v == 0.f) return false;
    m_off += m_v * dt;
    m_v *= std::exp(-m_p.decay * dt);
    if (std::abs(m_v) < m_p.minVelocity) m_v = 0.f;
    if (overscroll() != 0.f) {
        haptic(HapticKind::Edge);
        if (!m_p.overscroll) { m_off = clamped(); m_v = 0.f; return false; }
    }
    return m_v != 0.f || overscroll() != 0.f;
}

// ---- kinetic scroll area ----------------------------------------------------------------------------------------------
namespace {
struct KState {
    KineticScroller k;
    f32 t = 0.f;
    bool down = false, dragging = false, swallowClick = false;
    Vec2 downPos;
    TimerHandle timer;
};
struct KPool {
    struct Slot { NodeHandle node; std::unique_ptr<KState> s; };
    std::vector<Slot> slots;
    KState* get(const UiContext& ui, NodeHandle n) {
        const u64 k = ui.alive(n) ? ui.userData(n) : 0;
        if (k == 0 || k > slots.size() || slots[k - 1].node != n) return nullptr;
        return slots[k - 1].s.get();
    }
    KState& create(UiContext& ui, NodeHandle n) {
        for (std::size_t i = 0; i < slots.size(); ++i)
            if (!ui.alive(slots[i].node) || ui.userData(slots[i].node) != i + 1) {
                slots[i] = {n, std::make_unique<KState>()};
                ui.setUserData(n, i + 1);
                return *slots[i].s;
            }
        slots.push_back({n, std::make_unique<KState>()});
        ui.setUserData(n, slots.size());
        return *slots.back().s;
    }
};
KPool& kpool() { static KPool p; return p; }
constexpr f32 kTick = 1.f / 120.f;

void syncBounds(UiContext& ui, NodeHandle n, KState& s) {
    const f32 max = std::max(0.f, ui.scrollExtent(n).h - ui.contentRect(n).h);
    s.k.setBounds(0.f, max);
}
void apply(UiContext& ui, NodeHandle n, KState& s) { ui.scrollTo(n, {ui.scroll(n).x, s.k.clamped()}); ui.invalidate(n); }
void ensureTimer(UiContext& ui, NodeHandle n, KState& s) {
    if (s.timer.valid()) return;
    s.timer = ui.startTimer(kTick, true, [n](UiContext& u) {
        KState* st = kpool().get(u, n);
        if (!st) return;
        st->t += kTick;
        const bool moving = st->k.step(kTick);
        apply(u, n, *st);
        if (!moving && !st->down) {
            u.cancelTimer(st->timer);
            st->timer = {};
        }
    });
}

struct KineticW {
    static constexpr std::string_view kName = "kineticscroll";
    static constexpr A11yRole kRole = A11yRole::ScrollArea;
    static bool onEvent(EventContext& ec, const Event& e) {
        if (ec.phase == Phase::Bubble) return false;
        UiContext& ui = ec.ui;
        KState* s = kpool().get(ui, ec.node);
        if (!s) return false;
        switch (e.type) {
        case EventType::PointerDown:
            if (e.button != MouseButton::Left) return false;
            syncBounds(ui, ec.node, *s);
            s->k.setOffset(ui.scroll(ec.node).y + s->k.overscroll());
            s->down = true;
            s->dragging = false;
            s->downPos = e.pos;
            s->swallowClick = false;
            s->k.press(e.pos.y, s->t);
            ensureTimer(ui, ec.node, *s);
            return false; // children still see the press
        case EventType::PointerMove:
            if (!s->down) return false;
            if (!s->dragging && std::abs(e.pos.y - s->downPos.y) < s->k.profile().dragThreshold) return false;
            s->dragging = true;
            s->k.drag(e.pos.y, s->t);
            apply(ui, ec.node, *s);
            return true; // the drag belongs to the scroller now
        case EventType::PointerUp:
            if (!s->down) return false;
            s->down = false;
            s->k.release(s->t);
            if (s->dragging) { s->dragging = false; s->swallowClick = true; } // the core's Click follows the Up
            return false;
        case EventType::Click:
            if (!s->swallowClick) return false;
            s->swallowClick = false;
            return true; // no click after a drag
        case EventType::Wheel: {
            syncBounds(ui, ec.node, *s);
            return false; // the core wheel-scroll stays (desktop feel)
        }
        default: return false;
        }
    }
};
} // namespace

void registerPlatformFeel(UiContext& ui) {
    if (ui.registry().find("kineticscroll") != 0xFFFFFFFFu) return;
    ui.registry().registerWidget<KineticW>();
}

NodeHandle kineticScrollArea(UiContext& ui, NodeHandle parent, const ScrollProfile& profile, std::string_view id) {
    registerPlatformFeel(ui);
    const NodeHandle n = ui.create("kineticscroll", parent, id).value_or(NodeHandle{});
    if (!ui.alive(n)) return n;
    ui.setCss(n, "overflow", "scroll");
    ui.setCss(n, "flex-direction", "column");
    KState& s = kpool().create(ui, n);
    s.k.setProfile(profile);
    return n;
}

const KineticScroller* kineticScroller(const UiContext& ui, NodeHandle area) {
    KState* s = kpool().get(ui, area);
    return s ? &s->k : nullptr;
}

} // namespace fuse::ui::widgets
