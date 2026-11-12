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

} // namespace fuse::ui::widgets::detail
