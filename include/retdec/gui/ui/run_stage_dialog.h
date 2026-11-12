/**
 * @file include/retdec/gui/ui/run_stage_dialog.h
 * @brief Analysis → Run Stage…: "Tool to run on the loaded binary:" with the
 *        five choices of the Qt QInputDialog::getItem (Decompile, Fast
 *        decompile, fileinfo, unpacker, Signature Studio), OK / Cancel.
 *
 * The main window asks for a binary first when none is open (as the Qt
 * window did) and dispatches the chosen model::RunStageChoice.
 *
 * @copyright (c) 2026 Odin Loch trading as Imortek
 */

#ifndef RETDEC_GUI_UI_RUN_STAGE_DIALOG_H
#define RETDEC_GUI_UI_RUN_STAGE_DIALOG_H

#include "retdec/gui/model/dlg_run_options.h"
#include "retdec/gui/ui/prompt_dialog.h"

#include <functional>
#include <string>

namespace retdec::gui::ui {

class RunStageDialog {
public:
	explicit RunStageDialog(fuse::ui::UiContext& ui);

	/// Opens with the first item selected.
	void show(std::function<void(model::RunStageChoice choice)> onAccepted);
	void accept();
	void reject();
	bool isOpen() const
	{
		return dlg_.isOpen();
	}

	std::string title() const
	{
		return dlg_.title();
	}
	std::string label() const;
	int currentIndex() const;
	void setCurrentIndex(int index);
	std::string currentText() const;
	fuse::ui::NodeHandle combo() const
	{
		return combo_;
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

private:
	ModalDialog dlg_;
	fuse::ui::NodeHandle label_, combo_, ok_, cancel_;
	std::function<void(model::RunStageChoice)> onAccepted_;
};

} // namespace retdec::gui::ui

#endif // RETDEC_GUI_UI_RUN_STAGE_DIALOG_H
