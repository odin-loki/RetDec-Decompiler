/**
 * @file include/retdec/gui/ui/raw_options_dialog.h
 * @brief Analysis → Raw image / archive options…: --endian, --bit-size,
 *        --raw-section-vma, --ar-name and --ar-index for the next run.
 *
 * Fields (Qt object names): Endian (-e) combo omit / little / big
 * (rawOptEndian), Bit size (-b) combo omit / 16 / 32 / 64 (rawOptBitSize),
 * Raw section VMA "0x100000 (empty = omit)" (rawOptVma), Archive member
 * (--ar-name) "member.o (empty = omit)" (rawOptArName), Archive member index
 * (--ar-index) "0 (empty = omit; ignored if name is set)" (rawOptArIndex).
 * OK maps them with model::rawArchiveOptionsFromForm and reports "Raw /
 * archive options applied to the next decompile".
 *
 * @copyright (c) 2026 Odin Loch trading as Imortek
 */

#ifndef RETDEC_GUI_UI_RAW_OPTIONS_DIALOG_H
#define RETDEC_GUI_UI_RAW_OPTIONS_DIALOG_H

#include "retdec/gui/model/dlg_run_options.h"
#include "retdec/gui/ui/prompt_dialog.h"

#include <functional>
#include <string>

namespace retdec::gui::ui {

class RawOptionsDialog {
public:
	explicit RawOptionsDialog(fuse::ui::UiContext& ui);

	void show(const model::RawArchiveOptions& current, std::function<void(const model::RawArchiveOptions&)> onAccepted);
	void accept();
	void reject();
	bool isOpen() const
	{
		return dlg_.isOpen();
	}

	/// The fields as text / as options.
	model::RawArchiveForm form() const;
	model::RawArchiveOptions options() const
	{
		return model::rawArchiveOptionsFromForm(form());
	}
	void load(const model::RawArchiveOptions& o);

	std::string title() const
	{
		return dlg_.title();
	}
	fuse::ui::NodeHandle endianCombo() const
	{
		return endian_;
	}
	fuse::ui::NodeHandle bitSizeCombo() const
	{
		return bits_;
	}
	fuse::ui::NodeHandle vmaEdit() const
	{
		return vma_;
	}
	fuse::ui::NodeHandle arNameEdit() const
	{
		return arName_;
	}
	fuse::ui::NodeHandle arIndexEdit() const
	{
		return arIndex_;
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
	fuse::ui::NodeHandle endian_, bits_, vma_, arName_, arIndex_, ok_, cancel_;
	std::function<void(const model::RawArchiveOptions&)> onAccepted_;
};

} // namespace retdec::gui::ui

#endif // RETDEC_GUI_UI_RAW_OPTIONS_DIALOG_H
