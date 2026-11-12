// Platform feel (plan UI10): kinetic scroll physics with per-platform profiles, overscroll rubber-banding and
// spring-back, and a haptics hook. Pure math in KineticScroller (testable without a UI); kineticScrollArea() wires
// it to a scroll container (drag anywhere inside to scroll, release to fling; child clicks still work below the
// drag threshold).
#pragma once

#include "fuse/ui/ui.hpp"

#include <vector>

namespace fuse::ui::widgets {

enum class HapticKind : u8 { Tick, Edge, Toggle, Confirm, Reject };
/// Process-wide haptics sink (an app installs the platform one: Android Vibrator / iOS UIFeedbackGenerator / a
/// gamepad rumble). Null = no-op. Widgets call haptic() at selection ticks, scroll edges and toggles.
using HapticsFn = void (*)(void* user, HapticKind kind);
void setHapticsHandler(HapticsFn fn, void* user);
void haptic(HapticKind kind);

struct ScrollProfile {
    f32 decay = 4.5f;           ///< fling velocity decays as exp(-decay * t)
    f32 minVelocity = 8.f;      ///< px/s: below this a fling stops
    f32 maxVelocity = 8000.f;   ///< px/s clamp on release
    f32 sampleWindow = 0.1f;    ///< s of drag history used for the release velocity
    bool overscroll = true;     ///< rubber-band past the ends while dragging / flinging
    f32 rubberBand = 0.55f;     ///< iOS-style resistance coefficient
    f32 springStiffness = 180.f;///< spring-back (critically damped)
    f32 dragThreshold = 6.f;    ///< px before a press becomes a drag
    static ScrollProfile desktop(); ///< short glide, no overscroll
    static ScrollProfile ios();     ///< long glide, rubber band + bounce
    static ScrollProfile android(); ///< medium glide, edge clamp (glow is visual only)
};

class KineticScroller {
public:
    explicit KineticScroller(ScrollProfile p = ScrollProfile::ios()) : m_p(p) {}
    void setProfile(const ScrollProfile& p) { m_p = p; }
    const ScrollProfile& profile() const { return m_p; }
    void setBounds(f32 lo, f32 hi) { m_lo = lo; m_hi = hi < lo ? lo : hi; }
    void setOffset(f32 o) { m_off = o; m_v = 0.f; }
    /// pointer position along the scroll axis (content moves with the finger: offset = start + (pressPos - pos))
    void press(f32 pos, f32 t);
    void drag(f32 pos, f32 t);
    void release(f32 t);
    /// Advance the fling / spring by dt; returns true while still moving.
    bool step(f32 dt);
    f32 offset() const { return m_off; }        ///< may lie outside [lo, hi] while overscrolled
    f32 clamped() const { return m_off < m_lo ? m_lo : (m_off > m_hi ? m_hi : m_off); }
    f32 overscroll() const { return m_off - clamped(); }
    f32 velocity() const { return m_v; }
    bool dragging() const { return m_pressed; }
    bool active() const { return m_pressed || m_v != 0.f || overscroll() != 0.f; }

private:
    f32 rubber(f32 raw) const;
    ScrollProfile m_p;
    f32 m_lo = 0.f, m_hi = 0.f, m_off = 0.f, m_v = 0.f;
    bool m_pressed = false;
    f32 m_pressPos = 0.f, m_pressOff = 0.f;
    struct Sample { f32 t, pos; };
    std::vector<Sample> m_samples;
};

/// Vertical kinetic scroll container (role ScrollArea). The physics run on a context timer only while moving, so an
/// idle area costs nothing.
NodeHandle kineticScrollArea(UiContext& ui, NodeHandle parent, const ScrollProfile& profile = ScrollProfile::ios(), std::string_view id = {});
const KineticScroller* kineticScroller(const UiContext& ui, NodeHandle area);
void registerPlatformFeel(UiContext& ui);

} // namespace fuse::ui::widgets
