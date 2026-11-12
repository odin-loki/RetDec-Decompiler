/**
 * @file src/gui/ui/run_stage_dialog.cpp
 * @copyright (c) 2026 Odin Loch trading as Imortek
 */

#include "retdec/gui/ui/run_stage_dialog.h"

#include <fuse/ui/widgets/widgets.hpp>

namespace retdec::gui::ui {

namespace w = fuse::ui::widgets;
using namespace fuse::ui;

RunStageDialog::RunStageDialog(UiContext& ui): dlg_(ui, "Run stage", 420.f, 0.f, "runStageDialog")
{
	label_ = w::label(ui, dlg_.body(), "Tool to run on the loaded binary:");
	combo_ = w::comboBox(ui, dlg_.body(), dlg::joinLines(model::runStageItems()), 0);
	const NodeHandle buttons = dlg::buttonRow(ui, dlg_.body());
	ok_ = w::button(ui, buttons, "OK");
	ui.addClass(ok_, "primary");
	cancel_ = w::button(ui, buttons, "Cancel");
	ui.connect(ok_, SignalKind::Clicked, [this](UiContext&, NodeHandle, const SignalArgs&) { accept(); });
	ui.connect(cancel_, SignalKind::Clicked, [this](UiContext&, NodeHandle, const SignalArgs&) { reject(); });
}

void RunStageDialog::show(std::function<void(model::RunStageChoice)> onAccepted)
{
	onAccepted_ = std::move(onAccepted);
	setCurrentIndex(0);
	dlg_.open();
	dlg_.ui().setFocus(combo_, FocusReason::Keyboard);
}

void RunStageDialog::accept()
{
	const auto choice = model::runStageFromIndex(currentIndex());
	dlg_.close();
	if (choice && onAccepted_) onAccepted_(*choice);
}

void RunStageDialog::reject()
{
	dlg_.close();
}

std::string RunStageDialog::label() const
{
	return std::string(dlg_.ui().text(label_));
}

int RunStageDialog::currentIndex() const
{
	return dlg::indexOf(dlg_.ui(), combo_);
}

void RunStageDialog::setCurrentIndex(int index)
{
	dlg::setIndex(dlg_.ui(), combo_, index);
}

std::string RunStageDialog::currentText() const
{
	return std::string(w::comboText(dlg_.ui(), combo_));
}

} // namespace retdec::gui::ui
