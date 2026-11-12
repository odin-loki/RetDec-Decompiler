/**
 * @file include/retdec/gui/ui/c_style_dialog.h
 * @brief Analysis → C output style…: backend C flags for the next run.
 *
 * Checkboxes: Keep all brackets, No time-varying comments, No variable
 * renaming, No compound operators, No symbolic names, Disable backend
 * optimizations (--backend-no-opts); "Call-info obtainer" combo (omit / optim
 * / pessim); "Disabled backend opts" and "Enabled backend opts" fields
 * (comma-separated). OK hands back a model::CStyleOptions and reports "C output
 * style applied to the next decompile"; Cancel discards the edits.
 *
 * @copyright (c) 2026 Odin Loch trading as Imortek
 */

#ifndef RETDEC_GUI_UI_C_STYLE_DIALOG_H
#define RETDEC_GUI_UI_C_STYLE_DIALOG_H

#include "retdec/gui/model/dlg_run_options.h"
#include "retdec/gui/ui/prompt_dialog.h"

#include <array>
#include <functional>
#include <string>
#include <string_view>

namespace retdec::gui::ui {

class CStyleDialog {
public:
	/// Checkbox order (and object names of the Qt dialog).
	enum Check
	{
		KeepBrackets, ///< cStyleKeepBrackets
		NoTime,       ///< cStyleNoTime
		NoRename,     ///< cStyleNoRename
		NoCompound,   ///< cStyleNoCompound
		NoSymbolic,   ///< cStyleNoSymbolic
		NoOpts,       ///< cStyleNoOpts
		kCheckCount
	};

	explicit CStyleDialog(fuse::ui::UiContext& ui);

	/// Opens with the fields loaded from @p current.
	void show(const model::CStyleOptions& current, std::function<void(const model::CStyleOptions&)> onAccepted);
	void accept();
	void reject();
	bool isOpen() const
	{
		return dlg_.isOpen();
	}

	/// What the fields hold now.
	model::CStyleOptions options() const;
	void load(const model::CStyleOptions& o);

	std::string title() const
	{
		return dlg_.title();
	}
	fuse::ui::NodeHandle check(Check c) const
	{
		return checks_[std::size_t(c)];
	}
	fuse::ui::NodeHandle callInfoCombo() const
	{
		return callInfo_;
	}
	fuse::ui::NodeHandle disabledOptsEdit() const
	{
		return disabled_;
	}
	fuse::ui::NodeHandle enabledOptsEdit() const
	{
		return enabled_;
	}
	fuse::ui::NodeHandle okButton() const
	{
		return ok_;
	}
	fuse::ui::NodeHandle cancelButton() const
	{
		return cancel_;
	}
	fuse::ui::NodeHandle card() const
	{
		return dlg_.card();
	}

	std::function<void(const std::string& message, int timeoutMs)> onStatusMessage;

private:
	ModalDialog dlg_;
	std::array<fuse::ui::NodeHandle, kCheckCount> checks_{};
	fuse::ui::NodeHandle callInfo_, disabled_, enabled_, ok_, cancel_;
	std::function<void(const model::CStyleOptions&)> onAccepted_;
};

} // namespace retdec::gui::ui

#endif // RETDEC_GUI_UI_C_STYLE_DIALOG_H
