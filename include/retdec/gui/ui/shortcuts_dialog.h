/**
 * @file include/retdec/gui/ui/shortcuts_dialog.h
 * @brief Help → Keyboard Shortcuts: the Qt dialog's grouped table (File,
 *        Analysis, View, Editor / panels) in a scroll area, with Close.
 * @copyright (c) 2026 Odin Loch trading as Imortek
 */

#ifndef RETDEC_GUI_UI_SHORTCUTS_DIALOG_H
#define RETDEC_GUI_UI_SHORTCUTS_DIALOG_H

#include "retdec/gui/ui/prompt_dialog.h"

#include <functional>
#include <string>
#include <utility>
#include <vector>

namespace retdec::gui::ui {

class ShortcutsDialog {
public:
	explicit ShortcutsDialog(fuse::ui::UiContext& ui);

	void show(std::function<void()> onClosed = {});
	void close();
	bool isOpen() const { return dlg_.isOpen(); }

	std::string title() const { return dlg_.title(); }
	/// (keys, action) of every row shown, in order.
	std::vector<std::pair<std::string, std::string>> rows() const;
	/// Group titles shown, in order.
	std::vector<std::string> groups() const;
	fuse::ui::NodeHandle closeButton() const { return close_; }
	fuse::ui::NodeHandle card() const { return dlg_.card(); }

private:
	ModalDialog dlg_;
	fuse::ui::NodeHandle close_;
	std::vector<std::pair<fuse::ui::NodeHandle, fuse::ui::NodeHandle>> rows_;
	std::vector<fuse::ui::NodeHandle> groups_;
	std::function<void()> onClosed_;
};

} // namespace retdec::gui::ui

#endif // RETDEC_GUI_UI_SHORTCUTS_DIALOG_H
