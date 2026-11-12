/**
 * @file include/retdec/gui/ui/command_log_panel.h
 * @brief CommandLogPanel — append-only log of external tool invocations
 *        (timestamp, tool, cwd, exit code, timing, command line, output tail).
 *
 * Toolbar: Copy all, Clear, Save…; Ctrl+Shift+C copies everything while focus
 * is in the panel. Retained lines are capped (setMaxBlocks, default 20 000).
 *
 * @copyright (c) 2026 Odin Loch trading as Imortek
 */

#ifndef RETDEC_GUI_UI_COMMAND_LOG_PANEL_H
#define RETDEC_GUI_UI_COMMAND_LOG_PANEL_H

#include "retdec/gui/model/console_log.h"
#include "retdec/gui/ui/empty_state.h"
#include "retdec/gui/ui/tri_pane_code_view.h"

#include <fuse/ui/ui.hpp>

#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace retdec::gui::ui {

class CommandLogPanel {
public:
	CommandLogPanel(fuse::ui::UiContext& ui, fuse::ui::NodeHandle parent);
	~CommandLogPanel();
	CommandLogPanel(const CommandLogPanel&) = delete;
	CommandLogPanel& operator=(const CommandLogPanel&) = delete;

	fuse::ui::NodeHandle root() const
	{
		return root_;
	}
	std::string panelTitle() const
	{
		return "Command log";
	}

	void appendRun(
		std::string_view tool,
		const std::vector<std::string>& args,
		std::string_view cwd,
		int exitCode,
		int64_t elapsedMs,
		std::string_view outputTail);
	void clear();

	/// Cap on retained lines (minimum 100, default 20 000).
	void setMaxBlocks(int blocks);
	int maxBlocks() const;

	void copyAll();
	/// Writes the log to @p path; false (and onStatusMessage) on failure.
	bool saveTo(const std::filesystem::path& path);
	/// Opens a save dialog.
	void saveAs();

	std::function<void(const std::string& msg, int timeoutMs)> onStatusMessage;

	// Test hooks.
	std::string text() const
	{
		return buffer_.text();
	}
	int blockCount() const
	{
		return int(buffer_.lineCount());
	}
	bool isEmpty() const
	{
		return buffer_.isEmpty();
	}
	CodeView& view()
	{
		return *view_;
	}
	fuse::ui::NodeHandle copyButton() const
	{
		return copyBtn_;
	}
	fuse::ui::NodeHandle clearButton() const
	{
		return clearBtn_;
	}
	fuse::ui::NodeHandle saveButton() const
	{
		return saveBtn_;
	}

private:
	bool handleEvent(fuse::ui::EventContext& ec, const fuse::ui::Event& e);

	fuse::ui::UiContext& ui_;
	PanelEventHandler handler_;
	fuse::ui::NodeHandle root_, copyBtn_, clearBtn_, saveBtn_, saveDlg_;
	std::unique_ptr<CodeView> view_;
	model::ConsoleBuffer buffer_{20'000};
};

} // namespace retdec::gui::ui

#endif // RETDEC_GUI_UI_COMMAND_LOG_PANEL_H
