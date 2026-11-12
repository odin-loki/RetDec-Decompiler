/**
 * @file include/retdec/gui/ui/about_dialog.h
 * @brief Help → About RetDec (the Qt QMessageBox::about text, rendered with
 *        widgets::markdownView) with an OK button.
 * @copyright (c) 2026 Odin Loch trading as Imortek
 */

#ifndef RETDEC_GUI_UI_ABOUT_DIALOG_H
#define RETDEC_GUI_UI_ABOUT_DIALOG_H

#include "retdec/gui/ui/prompt_dialog.h"

#include <functional>
#include <string>

namespace retdec::gui::ui {

class AboutDialog {
public:
	explicit AboutDialog(fuse::ui::UiContext& ui);

	/// Opens the dialog; @p onClosed runs when OK closes it.
	void show(std::function<void()> onClosed = {});
	void close();
	bool isOpen() const { return dlg_.isOpen(); }

	std::string title() const { return dlg_.title(); }
	/// The Markdown shown (model::aboutMarkdown()).
	const std::string& markdown() const { return markdown_; }
	fuse::ui::NodeHandle okButton() const { return ok_; }
	fuse::ui::NodeHandle markdownNode() const { return view_; }
	fuse::ui::NodeHandle card() const { return dlg_.card(); }

private:
	ModalDialog dlg_;
	std::string markdown_;
	fuse::ui::NodeHandle view_, ok_;
	std::function<void()> onClosed_;
};

} // namespace retdec::gui::ui

#endif // RETDEC_GUI_UI_ABOUT_DIALOG_H
