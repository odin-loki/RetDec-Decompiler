/**
 * @file src/gui/ui/shortcuts_dialog.cpp
 * @copyright (c) 2026 Odin Loch trading as Imortek
 */

#include "retdec/gui/ui/shortcuts_dialog.h"

#include "retdec/gui/model/dlg_help.h"

#include <fuse/ui/widgets/widgets.hpp>

namespace retdec::gui::ui {

namespace w = fuse::ui::widgets;
using namespace fuse::ui;

ShortcutsDialog::ShortcutsDialog(UiContext& ui): dlg_(ui, "Keyboard Shortcuts", 520.f, 480.f, "shortcutsDialog")
{
	const NodeHandle scroll = w::scrollArea(ui, dlg_.body());
	ui.setCss(scroll, "flex-grow", "1");
	ui.setCss(scroll, "gap", "4px");
	ui.setCss(scroll, "padding", "4px");
	for (const model::ShortcutGroup& g: model::keyboardShortcutGroups())
	{
		const NodeHandle h = w::label(ui, scroll, g.title);
		ui.addClass(h, "md-h3");
		ui.setCss(h, "padding", "6px 0px 2px 0px");
		groups_.push_back(h);
		for (const model::ShortcutEntry& e: g.entries)
		{
			const NodeHandle row = dlg::hrow(ui, scroll);
			ui.setCss(row, "align-items", "start");
			const NodeHandle k = w::label(ui, row, e.keys);
			ui.setCss(k, "width", "150px");
			ui.setCss(k, "flex-shrink", "0");
			ui.addClass(k, "retdec-strong");
			const NodeHandle a = w::label(ui, row, e.action);
			ui.setCss(a, "flex-grow", "1");
			ui.setCss(a, "flex-shrink", "1");
			ui.setCss(a, "min-width", "0px");
			rows_.emplace_back(k, a);
		}
	}
	const NodeHandle buttons = dlg::buttonRow(ui, dlg_.body());
	close_ = w::button(ui, buttons, "Close");
	ui.connect(close_, SignalKind::Clicked, [this](UiContext&, NodeHandle, const SignalArgs&) { close(); });
}

void ShortcutsDialog::show(std::function<void()> onClosed)
{
	onClosed_ = std::move(onClosed);
	dlg_.open();
	dlg_.ui().setFocus(close_, FocusReason::Keyboard);
}

void ShortcutsDialog::close()
{
	dlg_.close();
	if (auto cb = std::move(onClosed_))
	{
		onClosed_ = nullptr;
		cb();
	}
}

std::vector<std::pair<std::string, std::string>> ShortcutsDialog::rows() const
{
	std::vector<std::pair<std::string, std::string>> out;
	for (const auto& [k, a]: rows_)
		out.emplace_back(std::string(dlg_.ui().text(k)), std::string(dlg_.ui().text(a)));
	return out;
}

std::vector<std::string> ShortcutsDialog::groups() const
{
	std::vector<std::string> out;
	for (NodeHandle g: groups_)
		out.emplace_back(dlg_.ui().text(g));
	return out;
}

} // namespace retdec::gui::ui
