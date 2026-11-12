/**
 * @file src/gui/ui/address_menu.cpp
 * @copyright (c) 2026 Odin Loch trading as Imortek
 */

#include "retdec/gui/ui/address_menu.h"

#include <fuse/ui/widgets/widgets.hpp>

namespace retdec::gui::ui {

namespace w = fuse::ui::widgets;
using namespace fuse::ui;

ContextMenu::ContextMenu(UiContext& ui): ui_(ui) {}

ContextMenu::~ContextMenu()
{
	close();
}

void ContextMenu::open(NodeHandle owner, Vec2 at, std::vector<MenuAction> actions)
{
	close();
	actions_ = std::move(actions);
	std::string items;
	for (std::size_t i = 0; i < actions_.size(); ++i)
	{
		if (i) items += '\n';
		items += actions_[i].label;
	}
	scope_ = w::popupMenu(ui_, owner, items, at);
	if (!ui_.alive(scope_)) return;
	// Children of the menu card are one node per line (items and separators).
	const NodeHandle menu = ui_.firstChild(scope_);
	std::size_t i = 0;
	NodeHandle firstEnabled;
	for (NodeHandle n = ui_.firstChild(menu); ui_.alive(n) && i < actions_.size(); n = ui_.nextSibling(n), ++i)
	{
		if (actions_[i].label == "-") continue;
		if (!actions_[i].enabled) ui_.setDisabled(n, true);
		else if (!firstEnabled.valid()) firstEnabled = n;
	}
	if (firstEnabled.valid()) ui_.setFocus(firstEnabled, FocusReason::Keyboard);
	auto* self = this;
	ui_.connect(scope_, SignalKind::Submitted, [self](UiContext&, NodeHandle, const SignalArgs& a) {
		self->picked(std::size_t(a.value));
	});
}

bool ContextMenu::isOpen() const
{
	return ui_.alive(scope_) && w::popupOpen(ui_, scope_);
}

void ContextMenu::close()
{
	if (ui_.alive(scope_)) w::closePopup(ui_, scope_);
	scope_ = {};
}

std::vector<std::string> ContextMenu::labels() const
{
	std::vector<std::string> out;
	if (!isOpen()) return out;
	for (const MenuAction& a: actions_)
		out.push_back(a.label);
	return out;
}

bool ContextMenu::isEnabled(std::size_t index) const
{
	return isOpen() && index < actions_.size() && actions_[index].enabled && actions_[index].label != "-";
}

int ContextMenu::indexOf(std::string_view label) const
{
	for (std::size_t i = 0; i < actions_.size(); ++i)
		if (actions_[i].label == label) return int(i);
	return -1;
}

bool ContextMenu::trigger(std::size_t index)
{
	if (!isEnabled(index)) return false;
	MenuAction a = actions_[index];
	close();
	if (a.run) a.run();
	return true;
}

void ContextMenu::picked(std::size_t index)
{
	if (index >= actions_.size()) return;
	MenuAction a = actions_[index]; // the callback may reopen / replace the menu
	scope_ = {};
	if (a.enabled && a.label != "-" && a.run) a.run();
}

void copyAddressToClipboard(UiContext& ui, uint64_t address)
{
	ui.setClipboardText(model::formatAddressHex(address));
}

std::vector<MenuAction> addressMenuActions(UiContext& ui, std::optional<uint64_t> address, std::function<void(uint64_t)> goToFunction)
{
	std::vector<MenuAction> v;
	UiContext* uip = &ui;
	v.push_back({kCopyAddressLabel, address.has_value(), [uip, address] {
		             if (address) copyAddressToClipboard(*uip, *address);
	             }});
	v.push_back({kGoToFunctionLabel, address.has_value(), [address, goToFunction] {
		             if (address && goToFunction) goToFunction(*address);
	             }});
	return v;
}

} // namespace retdec::gui::ui
