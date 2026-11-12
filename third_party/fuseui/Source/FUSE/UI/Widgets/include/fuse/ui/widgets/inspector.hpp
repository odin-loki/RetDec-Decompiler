// UI8 runtime UI inspector overlay. Lives in the inspected context's overlay layer (no effect on the app's layout,
// pointer-events: none), so it works in a shipped build: toggle it, hover to see the node under the pointer, click
// to select, arrow keys walk the tree (Up = parent, Down = first child, Left / Right = siblings), Escape closes.
// The panel shows the selected node's class, id, path, box, a11y role, states and key computed styles, plus frame
// stats (nodes, style table, draw commands).
#pragma once

#include "fuse/ui/ui.hpp"

#include <string>

namespace fuse::ui::widgets {

class Inspector {
public:
    explicit Inspector(UiContext& ui) : m_ui(ui) {}
    ~Inspector() { setEnabled(false); }
    Inspector(const Inspector&) = delete;
    Inspector& operator=(const Inspector&) = delete;

    void setEnabled(bool on);
    bool enabled() const { return m_root.valid(); }
    /// Input while enabled (the app routes input here first; returns true when consumed — everything except
    /// pointer moves is consumed while the inspector is open, so the app is frozen under it).
    bool pointerMove(Vec2 p);
    bool pointerDown(Vec2 p);
    bool keyDown(Key k);

    /// Deepest visible app node at `p` (inspector nodes and the overlay layer excluded).
    NodeHandle pick(Vec2 p) const;
    void select(NodeHandle n);
    NodeHandle selected() const { return m_selected; }
    NodeHandle hovered() const { return m_hover; }
    bool owns(NodeHandle n) const; ///< n is one of the inspector's own nodes
    /// One-line-per-field description of a node (what the panel shows).
    std::string describe(NodeHandle n) const;
    /// Places the highlight boxes and refreshes the panel (call once per frame before UiContext::update; cheap when
    /// nothing changed).
    void update();

private:
    UiContext& m_ui;
    NodeHandle m_root, m_hoverBox, m_selBox, m_panel, m_title, m_body;
    NodeHandle m_hover, m_selected;
    std::string m_shown;
};

} // namespace fuse::ui::widgets
