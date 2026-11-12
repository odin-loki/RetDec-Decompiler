/**
 * @file include/retdec/gui/ui/goto_address_dialog.h
 * @brief View → Go to address… (Ctrl+G) and the code tabs' Go to line
 *        (Ctrl+L): one line edit, OK / Cancel. The field keeps its text between
 *        openings (the Qt dialogs were created once and re-shown).
 *
 * Invalid input reports "Not a valid address" / "Not a valid line number"
 * through onStatusMessage (the Qt status bar, 3 s) and leaves the dialog open.
 *
 * @copyright (c) 2026 Odin Loch trading as Imortek
 */

#ifndef RETDEC_GUI_UI_GOTO_ADDRESS_DIALOG_H
#define RETDEC_GUI_UI_GOTO_ADDRESS_DIALOG_H

#include "retdec/gui/ui/prompt_dialog.h"

#include <cstdint>
#include <functional>
#include <string>
#include <string_view>

namespace retdec::gui::ui {

/// Go to address: hex (0x…), octal (0…) or decimal, as QString::toULongLong(…, 0).
class GotoAddressDialog {
public:
	explicit GotoAddressDialog(fuse::ui::UiContext& ui);

	/// Opens with focus in the field; @p onAccepted gets the address.
	void show(std::function<void(uint64_t address)> onAccepted);
	void accept();
	void reject();
	bool isOpen() const { return dlg_.isOpen(); }

	std::string text() const;
	void setText(std::string_view text);
	std::string title() const { return dlg_.title(); }
	fuse::ui::NodeHandle edit() const { return edit_; }
	fuse::ui::NodeHandle okButton() const { return ok_; }
	fuse::ui::NodeHandle cancelButton() const { return cancel_; }
	fuse::ui::NodeHandle card() const { return dlg_.card(); }

	/// (message, timeout ms) for the status bar.
	std::function<void(const std::string& message, int timeoutMs)> onStatusMessage;

private:
	ModalDialog dlg_;
	fuse::ui::NodeHandle edit_, ok_, cancel_;
	std::function<void(uint64_t)> onAccepted_;
};

/// Go to line: a decimal line number >= 1.
class GotoLineDialog {
public:
	explicit GotoLineDialog(fuse::ui::UiContext& ui);

	void show(std::function<void(int line)> onAccepted);
	void accept();
	void reject();
	bool isOpen() const { return dlg_.isOpen(); }

	std::string text() const;
	void setText(std::string_view text);
	std::string title() const { return dlg_.title(); }
	fuse::ui::NodeHandle edit() const { return edit_; }
	fuse::ui::NodeHandle okButton() const { return ok_; }
	fuse::ui::NodeHandle cancelButton() const { return cancel_; }

	std::function<void(const std::string& message, int timeoutMs)> onStatusMessage;

private:
	ModalDialog dlg_;
	fuse::ui::NodeHandle edit_, ok_, cancel_;
	std::function<void(int)> onAccepted_;
};

} // namespace retdec::gui::ui

#endif // RETDEC_GUI_UI_GOTO_ADDRESS_DIALOG_H
