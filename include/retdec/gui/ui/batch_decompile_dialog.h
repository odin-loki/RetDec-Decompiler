/**
 * @file include/retdec/gui/ui/batch_decompile_dialog.h
 * @brief File → Batch Decompile…: collect binary paths, then Start.
 *
 * "Binaries are decompiled sequentially using the same settings as Run Full
 * Analysis." over a list of absolute paths (duplicates skipped), the hint
 * "Add at least one binary to enable Start." while it is empty, Add… /
 * Remove / Clear, and Start (disabled while empty) / Cancel. Start hands the
 * paths to the callback; the main window runs the queue
 * (model::batchDecompile*).
 *
 * @copyright (c) 2026 Odin Loch trading as Imortek
 */

#ifndef RETDEC_GUI_UI_BATCH_DECOMPILE_DIALOG_H
#define RETDEC_GUI_UI_BATCH_DECOMPILE_DIALOG_H

#include "retdec/gui/ui/prompt_dialog.h"

#include <functional>
#include <string>
#include <vector>

namespace retdec::gui::ui {

class BatchDecompileDialog {
public:
	explicit BatchDecompileDialog(fuse::ui::UiContext& ui);

	/// Opens with an empty list; @p onStart gets the paths when Start is pressed.
	void show(std::function<void(const std::vector<std::string>& paths)> onStart, std::string_view startDir = {});
	void start();
	void reject();
	bool isOpen() const
	{
		return dlg_.isOpen();
	}

	/// The list, in order (absolute paths).
	const std::vector<std::string>& binaryPaths() const
	{
		return paths_;
	}
	/// Add paths as the Add… file prompt does (absolute, duplicates skipped).
	void addPaths(const std::vector<std::string>& paths);
	/// Remove the selected row (no-op without a selection).
	void removeSelected();
	void clear();
	void setSelectedRow(int row);
	int selectedRow() const;
	bool startEnabled() const;
	bool hintVisible() const
	{
		return hintShown_;
	}

	std::string title() const
	{
		return dlg_.title();
	}
	fuse::ui::NodeHandle list() const
	{
		return list_;
	}
	fuse::ui::NodeHandle addButton() const
	{
		return add_;
	}
	fuse::ui::NodeHandle removeButton() const
	{
		return remove_;
	}
	fuse::ui::NodeHandle clearButton() const
	{
		return clear_;
	}
	fuse::ui::NodeHandle startButton() const
	{
		return start_;
	}
	fuse::ui::NodeHandle cancelButton() const
	{
		return cancel_;
	}
	FilePrompt& filePrompt()
	{
		return files_;
	}
	fuse::ui::NodeHandle card() const
	{
		return dlg_.card();
	}

private:
	void refresh();

	ModalDialog dlg_;
	FilePrompt files_;
	fuse::ui::NodeHandle list_, hint_, add_, remove_, clear_, start_, cancel_;
	std::vector<std::string> paths_;
	std::string startDir_;
	bool hintShown_ = true;
	std::function<void(const std::vector<std::string>&)> onStart_;
};

} // namespace retdec::gui::ui

#endif // RETDEC_GUI_UI_BATCH_DECOMPILE_DIALOG_H
