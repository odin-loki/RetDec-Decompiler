#include "fuse/ui/a11y/a11y.hpp"

#include <algorithm>
#include <unordered_map>

namespace fuse::ui::a11y {

namespace {
bool same(const A11ySnapshotNode& a, const A11ySnapshotNode& b, u64 pa, u64 pb) {
    return a.role == b.role && a.name == b.name && a.bounds.x == b.bounds.x && a.bounds.y == b.bounds.y &&
           a.bounds.w == b.bounds.w && a.bounds.h == b.bounds.h && a.focusable == b.focusable && a.focused == b.focused &&
           a.checked == b.checked && a.disabled == b.disabled && a.value == b.value && a.lo == b.lo && a.hi == b.hi && pa == pb;
}
} // namespace

u32 Bridge::sync() {
    std::lock_guard<std::mutex> lock(m_snapMutex);
    m_prev.swap(m_cur);
    m_ui.snapshotA11y(m_cur);
    std::unordered_map<u64, u32> prevIdx;
    prevIdx.reserve(m_prev.size());
    for (u32 i = 0; i < m_prev.size(); ++i) prevIdx.emplace(idOf(m_prev[i].node), i);
    auto parentId = [](const std::vector<A11ySnapshotNode>& v, u32 i) { return v[i].parent == ~0u ? 0ull : idOf(v[v[i].parent].node); };
    u32 changed = 0, matched = 0;
    for (u32 i = 0; i < m_cur.size(); ++i) {
        auto it = prevIdx.find(idOf(m_cur[i].node));
        if (it == prevIdx.end()) { ++changed; continue; }
        ++matched;
        if (!same(m_cur[i], m_prev[it->second], parentId(m_cur, i), parentId(m_prev, it->second))) ++changed;
    }
    changed += u32(m_prev.size()) - matched; // removed
    return changed;
}

u64 Bridge::focusId() const {
    for (const auto& n : m_cur)
        if (n.focused) return idOf(n.node);
    return m_cur.empty() ? 0 : idOf(m_cur[0].node);
}

std::vector<u32> Bridge::children(u32 i) const {
    std::vector<u32> c;
    for (u32 k = i + 1; k < m_cur.size(); ++k)
        if (m_cur[k].parent == i) c.push_back(k);
    return c;
}

bool Bridge::perform(const ActionRequest& r) {
    const NodeHandle n = nodeOf(r.target);
    if (!m_ui.alive(n)) return false;
    switch (r.action) {
    case Action::Focus: m_ui.setFocus(n, FocusReason::Keyboard); return true;
    case Action::Click: {
        const Rect b = m_ui.rect(n);
        const Vec2 p{b.x + b.w * 0.5f, b.y + b.h * 0.5f};
        m_ui.pointerMove(p);
        m_ui.pointerDown(p);
        m_ui.pointerUp(p);
        return true;
    }
    case Action::Increment:
    case Action::Decrement: {
        f32 lo = 0.f, hi = 0.f;
        for (const auto& s : m_cur)
            if (s.node == n) { lo = s.lo; hi = s.hi; }
        f32 step = (hi - lo) * 0.01f;
        if (step <= 0.f) step = 1.f;
        const f32 v = m_ui.value(n) + (r.action == Action::Increment ? step : -step);
        m_ui.setValue(n, hi > lo ? std::clamp(v, lo, hi) : v, true);
        return true;
    }
    case Action::SetValue: m_ui.setValue(n, r.value, true); return true;
    }
    return false;
}

void Bridge::enqueue(const ActionRequest& r) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_queue.push_back(r);
}

u32 Bridge::drain() {
    std::vector<ActionRequest> q;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        q.swap(m_queue);
    }
    u32 done = 0;
    for (const auto& r : q) done += perform(r) ? 1u : 0u;
    return done;
}

} // namespace fuse::ui::a11y
