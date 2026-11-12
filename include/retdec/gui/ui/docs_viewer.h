/**
 * @file include/retdec/gui/ui/docs_viewer.h
 * @brief Help → Documentation…: the Markdown documents shipped with RetDec,
 *        listed on the left and rendered with widgets::markdownView on the
 *        right.
 *
 * The Qt GUI opened README.md / WINDOWS_NATIVE_BUILD.md from
 * `<bin>/../share/retdec/doc/` (or a build tree's `docs/`) in the desktop's
 * browser; this viewer finds the same directories (model::findDocsDir(), also
 * `share/retdec/docs`) and shows every *.md there in-app, opening on the file
 * the Qt version preferred. With no documentation it shows the Qt message
 * "Could not find documentation under share/retdec/doc/ or docs/." instead.
 *
 * @copyright (c) 2026 Odin Loch trading as Imortek
 */

#ifndef RETDEC_GUI_UI_DOCS_VIEWER_H
#define RETDEC_GUI_UI_DOCS_VIEWER_H

#include "retdec/gui/model/dlg_help.h"
#include "retdec/gui/ui/prompt_dialog.h"

#include <filesystem>
#include <functional>
#include <string>
#include <vector>

namespace retdec::gui::ui {

class DocsViewer {
public:
	explicit DocsViewer(fuse::ui::UiContext& ui);

	/// Finds the docs (model::findDocsDir(), or the directory set with
	/// setDocsDir()) and opens the viewer; with none, opens the information
	/// box. Returns false in that case.
	bool show(std::function<void()> onClosed = {});
	void close();
	bool isOpen() const { return dlg_.isOpen(); }

	/// Use @p dir instead of searching (tests, a user-chosen folder). Empty restores the search.
	void setDocsDir(std::filesystem::path dir) { overrideDir_ = std::move(dir); }
	const std::filesystem::path& docsDir() const { return dir_; }
	const std::vector<model::DocEntry>& docs() const { return docs_; }

	/// Show document @p index (list selection follows).
	void showDoc(int index);
	int currentIndex() const { return current_; }
	/// The Markdown currently rendered.
	const std::string& currentMarkdown() const { return markdown_; }

	std::string title() const { return dlg_.title(); }
	fuse::ui::NodeHandle list() const { return list_; }
	fuse::ui::NodeHandle markdownNode() const { return view_; }
	fuse::ui::NodeHandle closeButton() const { return close_; }
	MessageDialog& message() { return message_; }
	fuse::ui::NodeHandle card() const { return dlg_.card(); }

private:
	ModalDialog dlg_;
	MessageDialog message_;
	fuse::ui::NodeHandle list_, scroll_, view_, pathLabel_, close_;
	std::filesystem::path overrideDir_, dir_;
	std::vector<model::DocEntry> docs_;
	int current_ = -1;
	std::string markdown_;
	std::function<void()> onClosed_;
};

} // namespace retdec::gui::ui

#endif // RETDEC_GUI_UI_DOCS_VIEWER_H
