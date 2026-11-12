/**
 * @file src/gui/ui/c_style_dialog.cpp
 * @copyright (c) 2026 Odin Loch trading as Imortek
 */

#include "retdec/gui/ui/c_style_dialog.h"

#include "retdec/gui/model/dlg_input.h"

#include <fuse/ui/widgets/widgets.hpp>

namespace retdec::gui::ui {

namespace w = fuse::ui::widgets;
using namespace fuse::ui;

CStyleDialog::CStyleDialog(UiContext& ui): dlg_(ui, "C output style", 400.f, 0.f, "cOutputStyleDialog")
{
	const NodeHandle b = dlg_.body();
	static constexpr std::array<std::pair<std::string_view, std::string_view>, kCheckCount> kChecks{{
		{"Keep all brackets", "cStyleKeepBrackets"},
		{"No time-varying comments", "cStyleNoTime"},
		{"No variable renaming", "cStyleNoRename"},
		{"No compound operators", "cStyleNoCompound"},
		{"No symbolic names", "cStyleNoSymbolic"},
		{"Disable backend optimizations (--backend-no-opts)", "cStyleNoOpts"},
	}};
	for (std::size_t i = 0; i < kChecks.size(); ++i)
		checks_[i] = w::checkBox(ui, b, kChecks[i].first, false, kChecks[i].second);
	w::label(ui, b, "Call-info obtainer");
	callInfo_ = w::comboBox(ui, b, dlg::joinLines(model::cStyleCallInfoItems()), 0, "cStyleCallInfo");
	w::label(ui, b, "Disabled backend opts");
	disabled_ = w::lineEdit(ui, b, "", "cStyleDisabledOpts");
	w::setPlaceholderText(ui, disabled_, "comma-separated --backend-disabled-opts");
	w::label(ui, b, "Enabled backend opts");
	enabled_ = w::lineEdit(ui, b, "", "cStyleEnabledOpts");
	w::setPlaceholderText(ui, enabled_, "comma-separated --backend-enabled-opts");
	const NodeHandle buttons = dlg::buttonRow(ui, b);
	ok_ = w::button(ui, buttons, "OK");
	ui.addClass(ok_, "primary");
	cancel_ = w::button(ui, buttons, "Cancel");
	ui.connect(ok_, SignalKind::Clicked, [this](UiContext&, NodeHandle, const SignalArgs&) { accept(); });
	ui.connect(cancel_, SignalKind::Clicked, [this](UiContext&, NodeHandle, const SignalArgs&) { reject(); });
}

void CStyleDialog::load(const model::CStyleOptions& o)
{
	UiContext& ui = dlg_.ui();
	ui.setChecked(checks_[KeepBrackets], o.keepAllBrackets);
	ui.setChecked(checks_[NoTime], o.noTimeVaryingInfo);
	ui.setChecked(checks_[NoRename], o.noVarRenaming);
	ui.setChecked(checks_[NoCompound], o.noCompoundOperators);
	ui.setChecked(checks_[NoSymbolic], o.noSymbolicNames);
	ui.setChecked(checks_[NoOpts], o.backendNoOpts);
	dlg::setIndex(ui, callInfo_, model::cStyleCallInfoIndex(o.callInfoObtainer));
	dlg::setEditText(ui, disabled_, o.backendDisabledOpts);
	dlg::setEditText(ui, enabled_, o.backendEnabledOpts);
}

model::CStyleOptions CStyleDialog::options() const
{
	const UiContext& ui = dlg_.ui();
	model::CStyleOptions o;
	o.keepAllBrackets = ui.checked(checks_[KeepBrackets]);
	o.noTimeVaryingInfo = ui.checked(checks_[NoTime]);
	o.noVarRenaming = ui.checked(checks_[NoRename]);
	o.noCompoundOperators = ui.checked(checks_[NoCompound]);
	o.noSymbolicNames = ui.checked(checks_[NoSymbolic]);
	o.backendNoOpts = ui.checked(checks_[NoOpts]);
	o.callInfoObtainer = model::cStyleCallInfoFromText(w::comboText(ui, callInfo_));
	o.backendDisabledOpts = std::string(model::dlgTrim(dlg::editText(ui, disabled_)));
	o.backendEnabledOpts = std::string(model::dlgTrim(dlg::editText(ui, enabled_)));
	return o;
}

void CStyleDialog::show(const model::CStyleOptions& current, std::function<void(const model::CStyleOptions&)> onAccepted)
{
	onAccepted_ = std::move(onAccepted);
	load(current);
	dlg_.open();
}

void CStyleDialog::accept()
{
	const model::CStyleOptions o = options();
	dlg_.close();
	if (onAccepted_) onAccepted_(o);
	if (onStatusMessage) onStatusMessage("C output style applied to the next decompile", 5000);
}

void CStyleDialog::reject()
{
	dlg_.close();
}

} // namespace retdec::gui::ui
