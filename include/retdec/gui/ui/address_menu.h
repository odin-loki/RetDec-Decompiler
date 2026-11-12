/**
 * @file include/retdec/gui/ui/address_menu.h
 * @brief Context menus of the code / table views: a small action-list popup
 *        on top of fuse::ui::widgets::popupMenu (disabled items, callbacks),
 *        and the shared "Copy Address" / "Go to Function" address menu.
 *        Address parsing / formatting lives in model/address.h and
 *        model/code_address.h.
 * @copyright (c) 2026 Odin Loch trading as Imortek
 */

#ifndef RETDEC_GUI_UI_ADDRESS_MENU_H
#define RETDEC_GUI_UI_ADDRESS_MENU_H

#include "retdec/gui/model/address.h"

#include <fuse/ui/ui.hpp>

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace retdec::gui::ui {

using model::kCopyAddressLabel;
using model::kGoToFunctionLabel;

/// One context-menu entry; label "-" is a separator.
struct MenuAction
{
	std::string label;
	bool enabled = true;
	std::function<void()> run;
};

/// A popup menu whose items run callbacks. One menu open at a time per
/// instance; the instance must outlive the popup (panels own one).
class ContextMenu {
public:
	explicit ContextMenu(fuse::ui::UiContext& ui);
	~ContextMenu();
	ContextMenu(const ContextMenu&) = delete;
	ContextMenu& operator=(const ContextMenu&) = delete;

	/// Opens at @p at (context coordinates). @p owner gets focus back on close
	/// and also receives the menu's Submitted signal, so pass a node with no
	/// Submitted handler of its own (a panel root).
	void open(fuse::ui::NodeHandle owner, fuse::ui::Vec2 at, std::vector<MenuAction> actions);
	bool isOpen() const;
	void close();

	// Test hooks: what the open menu shows, and picking an item like a click.
	std::vector<std::string> labels() const;
	bool isEnabled(std::size_t index) const;
	/// Runs item @p index (if enabled) and closes the menu; false when disabled / out of range.
	bool trigger(std::size_t index);
	/// Index of the item labelled @p label, or -1.
	int indexOf(std::string_view label) const;

private:
	void picked(std::size_t index);
	fuse::ui::UiContext& ui_;
	fuse::ui::NodeHandle scope_;
	std::vector<MenuAction> actions_;
};

/// Puts formatAddressHex(@p address) on the context's clipboard (the app
/// installs the platform clipboard into the UiContext).
void copyAddressToClipboard(fuse::ui::UiContext& ui, uint64_t address);

/// "Copy Address" + "Go to Function" for @p address; both disabled when the
/// line under the pointer has no address.
std::vector<MenuAction> addressMenuActions(
	fuse::ui::UiContext& ui, std::optional<uint64_t> address, std::function<void(uint64_t)> goToFunction);

} // namespace retdec::gui::ui

#endif // RETDEC_GUI_UI_ADDRESS_MENU_H
