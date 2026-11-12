/**
 * @file src/gui/ui/goto_address_dialog.cpp
 * @copyright (c) 2026 Odin Loch trading as Imortek
 */

#include "retdec/gui/ui/goto_address_dialog.h"

#include "retdec/gui/model/dlg_input.h"

#include <fuse/ui/widgets/widgets.hpp>

namespace retdec::gui::ui {

namespace w = fuse::ui::widgets;
using namespace fuse::ui;

namespace {

/// Field + "e.g." note + OK / Cancel, wired to the owner's accept / reject.
template <class D>
void buildSingleField(UiContext& ui, ModalDialog& dlg, std::string_view example, NodeHandle& edit, NodeHandle& ok,
                      NodeHandle& cancel, D* owner)
{
	edit = w::lineEdit(ui, dlg.body(), "");
	dlg::noteLabel(ui, dlg.body(), example);
	const NodeHandle buttons = dlg::buttonRow(ui, dlg.body());
	ok = w::button(ui, buttons, "OK");
	ui.addClass(ok, "primary");
	cancel = w::button(ui, buttons, "Cancel");
	ui.connect(ok, SignalKind::Clicked, [owner](UiContext&, NodeHandle, const SignalArgs&) { owner->accept(); });
	ui.connect(cancel, SignalKind::Clicked, [owner](UiContext&, NodeHandle, const SignalArgs&) { owner->reject(); });
	ui.connect(edit, SignalKind::Submitted, [owner](UiContext&, NodeHandle, const SignalArgs&) { owner->accept(); });
}

} // namespace

// ── GotoAddressDialog ────────────────────────────────────────────────────────

GotoAddressDialog::GotoAddressDialog(UiContext& ui): dlg_(ui, "Go to address", 360.f, 0.f, "gotoAddressDialog")
{
	buildSingleField(ui, dlg_, "e.g. 0x401000", edit_, ok_, cancel_, this);
}

void GotoAddressDialog::show(std::function<void(uint64_t)> onAccepted)
{
	onAccepted_ = std::move(onAccepted);
	dlg_.open();
	dlg_.ui().setFocus(edit_, FocusReason::Keyboard);
}

void GotoAddressDialog::accept()
{
	const auto addr = model::dlgParseGotoAddress(text());
	if (!addr)
	{
		if (onStatusMessage) onStatusMessage(addr.error(), 3000);
		return;
	}
	dlg_.close();
	if (onAccepted_) onAccepted_(*addr);
}

void GotoAddressDialog::reject()
{
	dlg_.close();
}

std::string GotoAddressDialog::text() const
{
	return dlg::editText(dlg_.ui(), edit_);
}

void GotoAddressDialog::setText(std::string_view text)
{
	dlg::setEditText(dlg_.ui(), edit_, text);
}

// ── GotoLineDialog ───────────────────────────────────────────────────────────

GotoLineDialog::GotoLineDialog(UiContext& ui): dlg_(ui, "Go to line", 360.f, 0.f, "gotoLineDialog")
{
	buildSingleField(ui, dlg_, "e.g. 1", edit_, ok_, cancel_, this);
}

void GotoLineDialog::show(std::function<void(int)> onAccepted)
{
	onAccepted_ = std::move(onAccepted);
	dlg_.open();
	dlg_.ui().setFocus(edit_, FocusReason::Keyboard);
}

void GotoLineDialog::accept()
{
	const auto line = model::dlgParseGotoLine(text());
	if (!line)
	{
		if (onStatusMessage) onStatusMessage(line.error(), 3000);
		return;
	}
	dlg_.close();
	if (onAccepted_) onAccepted_(*line);
}

void GotoLineDialog::reject()
{
	dlg_.close();
}

std::string GotoLineDialog::text() const
{
	return dlg::editText(dlg_.ui(), edit_);
}

void GotoLineDialog::setText(std::string_view text)
{
	dlg::setEditText(dlg_.ui(), edit_, text);
}

} // namespace retdec::gui::ui
