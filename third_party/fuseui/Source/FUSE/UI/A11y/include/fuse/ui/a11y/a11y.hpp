// fuse_ui_a11y: platform accessibility bridge (plan D7). The core keeps an accessible tree (roles, names, states,
// values); this module snapshots it, diffs successive snapshots, maps platform actions back onto the UiContext and,
// when built with FUSE_UI_ACCESSKIT_DIR, feeds AccessKit (AT-SPI on Linux; UIA / NSAccessibility through the same
// tree update on Windows / macOS once their adapters are wired to a native window).
#pragma once

#include "fuse/ui/ui.hpp"

#include <mutex>
#include <string>
#include <vector>

namespace fuse::ui::a11y {

/// Stable platform id for a node: generation in the high half, slot in the low half (never 0).
inline u64 idOf(NodeHandle n) { return (u64(n.generation) << 32) | u64(n.index + 1u); }
inline NodeHandle nodeOf(u64 id) { return {u32(id & 0xFFFFFFFFu) - 1u, u32(id >> 32)}; }

enum class Action : u8 { Focus, Click, Increment, Decrement, SetValue };
struct ActionRequest {
    Action action = Action::Focus;
    u64 target = 0;
    f32 value = 0.f; ///< SetValue
};

class Bridge {
public:
    explicit Bridge(UiContext& ui) : m_ui(ui) {}
    /// Snapshot the tree; returns the number of nodes added, removed or changed since the last sync (0 = no update
    /// needed). The first sync reports every node.
    u32 sync();
    const std::vector<A11ySnapshotNode>& nodes() const { return m_cur; }
    u64 focusId() const;
    /// Children of snapshot entry i (indices into nodes()).
    std::vector<u32> children(u32 i) const;
    /// Apply an action on the UI thread. Click synthesises a pointer press at the node centre; Increment /
    /// Decrement step the value by its step (or 1 % of the range); SetValue clamps. Returns false for dead nodes.
    bool perform(const ActionRequest& r);
    /// Thread-safe queue for adapters whose callbacks run on another thread; drain() applies them on the UI thread.
    void enqueue(const ActionRequest& r);
    u32 drain();
    UiContext& ui() { return m_ui; }
    /// Held while the snapshot is rebuilt (sync) or read from an adapter thread (activation callbacks).
    std::unique_lock<std::mutex> lockSnapshot() { return std::unique_lock<std::mutex>(m_snapMutex); }

private:
    UiContext& m_ui;
    std::vector<A11ySnapshotNode> m_cur, m_prev;
    std::mutex m_mutex, m_snapMutex;
    std::vector<ActionRequest> m_queue;
};

/// True when this build links AccessKit.
bool accessKitAvailable();

#if FUSE_UI_HAVE_ACCESSKIT
/// Build an AccessKit tree update (caller owns; pass to an adapter or accesskit_tree_update_free). Returned as void*
/// so this header does not pull in accesskit.h.
void* buildAccessKitUpdate(const Bridge& b, bool includeTree = true);
std::string accessKitDebug(const void* update); ///< AccessKit's debug dump of an update
void freeAccessKitUpdate(void* update);
/// AccessKit's Linux AT-SPI adapter driven by a Bridge: activation builds the full tree, actions are queued on the
/// bridge (drain them each frame), update() pushes a fresh tree when sync() saw changes and an AT is listening.
class UnixAdapter {
public:
    explicit UnixAdapter(Bridge& b);
    ~UnixAdapter();
    UnixAdapter(const UnixAdapter&) = delete;
    UnixAdapter& operator=(const UnixAdapter&) = delete;
    void update();                 ///< call once per frame after ui.update()
    void setWindowFocused(bool focused);
    bool valid() const { return m_adapter != nullptr; }

private:
    Bridge& m_bridge;
    void* m_adapter = nullptr;
};
#endif

} // namespace fuse::ui::a11y
