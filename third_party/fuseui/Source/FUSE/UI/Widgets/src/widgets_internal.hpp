// Internal glue between the Tier A and Tier B widget sources.
#pragma once

#include "fuse/ui/ui.hpp"

namespace fuse::ui::widgets::detail {

/// Opens the combo box's popup list (Tier B). False when the Tier B classes are not registered (Tier A cycles).
bool openComboPopup(UiContext& ui, NodeHandle combo);
void registerTierB2(UiContext& ui);
/// Popup menu whose item values start at `base` (submenus report the line index in the root list).
NodeHandle popupMenuAt(UiContext& ui, NodeHandle owner, std::string_view items, Vec2 at, u32 base, std::string_view id = {});
/// A popup scope (outside click / Escape closes it) holding an empty card at `at`; returns the card.
NodeHandle popupPanel(UiContext& ui, NodeHandle owner, Vec2 at);

/// Style of sub-part `part` of the node being painted (UiContext::partStyle), starting from the built-in look:
/// `background`, `border` and `color` (transparent `color` = the owner's text colour); font size / family / weight
/// come from the owner. Style sheets override any of it (`tableheader { background: ... }`).
inline ComputedStyle partOf(const PaintContext& pc, std::string_view part, u32 states, Color background, Color border = {},
                            Color color = {}) {
    ComputedStyle d;
    d.background = background;
    d.borderColor = border;
    d.color = color.a > 0.f ? color : pc.style.color;
    d.background2 = pc.style.background2;
    d.fontSize = pc.style.fontSize;
    d.fontFamily = pc.style.fontFamily;
    d.fontFamilyCount = pc.style.fontFamilyCount;
    d.fontWeight = pc.style.fontWeight;
    return pc.ui.partStyle(pc.node, part, states, &d);
}

} // namespace fuse::ui::widgets::detail
