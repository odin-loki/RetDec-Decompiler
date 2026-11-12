/**
 * @file include/retdec/gui/ui/live_console_panel.h
 * @brief LiveConsolePanel — streaming stdout / stderr of retdec CLI children.
 *
 * Producers (process reader threads, run by the model) push bytes into the
 * panel's thread-safe ConsoleFeed for an attached source; a UI timer
 * (~60 Hz by default) drains at most maxBytesPerFlush bytes per stream per
 * tick, so a chatty `--print-after-all` run never blocks a frame. The text
 * is a bounded line buffer (maxBlocks, oldest lines dropped), ANSI escapes
 * are stripped, invalid UTF-8 is repaired, and lines are coloured by content:
 * errors red, warnings yellow, "==> " banners blue, "<== " footers green.
 *
 * Toolbar: Copy all, Save…, Clear, Auto-scroll (follow tail), status label.
 * Ctrl+Shift+C copies everything while focus is in the panel.
 *
 * @copyright (c) 2026 Odin Loch trading as Imortek
 */

#ifndef RETDEC_GUI_UI_LIVE_CONSOLE_PANEL_H
#define RETDEC_GUI_UI_LIVE_CONSOLE_PANEL_H

#include "retdec/gui/model/console_log.h"
#include "retdec/gui/ui/empty_state.h"
#include "retdec/gui/ui/tri_pane_code_view.h"

#include <fuse/ui/ui.hpp>

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace retdec::gui::ui {

class LiveConsolePanel {
public:
	using Stream = model::ConsoleStream;

	LiveConsolePanel(fuse::ui::UiContext& ui, fuse::ui::NodeHandle parent);
	~LiveConsolePanel();
	LiveConsolePanel(const LiveConsolePanel&) = delete;
	LiveConsolePanel& operator=(const LiveConsolePanel&) = delete;

	fuse::ui::NodeHandle root() const
	{
		return root_;
	}
	std::string panelTitle() const
	{
		return "Console";
	}

	// ── Live sources (the Qt attachProcess / detachProcess) ─────────────────
	/// Registers a running tool; @p label names it ("retdec-decompiler").
	uint64_t attachSource(std::string_view label);
	/// The tool finished: everything still pending is shown now, then the source is removed.
	void detachSource(uint64_t source);
	/// Any thread: queues output of @p source (shown on a later flush tick).
	void pushOutput(uint64_t source, Stream stream, std::string_view bytes);
	/// The feed itself, for producers that may outlive the panel (thread-safe).
	std::shared_ptr<model::ConsoleFeed> feed() const
	{
		return feed_;
	}

	// ── Direct text ingress ──────────────────────────────────────────────────
	void appendBanner(std::string_view tool, const std::vector<std::string>& args, std::string_view cwd);
	void appendFooter(std::string_view tool, int exitCode, int64_t elapsedMs);
	/// Inserts immediately (not capped): ANSI stripped, UTF-8 repaired.
	void appendChunk(Stream stream, std::string_view bytes);
	/// appendChunk with a trailing newline added when missing.
	void appendLine(Stream stream, std::string_view line);

	// ── Configuration ────────────────────────────────────────────────────────
	/// Retained lines (minimum 1000, default 50 000).
	void setMaxBlocks(int blocks);
	int maxBlocks() const;
	/// Flush timer rate, clamped to 2..250 Hz.
	void setFlushHz(int hz);
	int flushHz() const
	{
		return flushHz_;
	}
	/// Bytes inserted per stream per flush tick (minimum 4 KiB, default 16 KiB).
	void setMaxBytesPerFlush(int bytes);
	int maxBytesPerFlush() const
	{
		return maxBytesPerFlush_;
	}
	void setAutoScroll(bool on);
	bool autoScroll() const
	{
		return autoScroll_;
	}

	bool isEmpty() const
	{
		return buffer_.isEmpty();
	}
	std::string text() const
	{
		return buffer_.text();
	}
	/// Lines held (QPlainTextEdit::blockCount semantics).
	int blockCount() const
	{
		return int(buffer_.lineCount());
	}
	const model::ConsoleBuffer& buffer() const
	{
		return buffer_;
	}

	// ── Actions ──────────────────────────────────────────────────────────────
	/// Drains pending source output (one capped tick).
	void flush();
	void copyAll();
	/// Saves the whole buffer (UTF-8). False on error, see lastError().
	bool saveAs(const std::filesystem::path& path);
	std::string lastError() const
	{
		return lastError_;
	}
	/// Opens a save dialog and saves there (status label reports the result).
	void saveAsDialog();
	/// Erases all output and pending bytes.
	void clear();

	// ── Test hooks ───────────────────────────────────────────────────────────
	CodeView& view()
	{
		return *view_;
	}
	std::string statusText() const;
	fuse::ui::NodeHandle copyButton() const
	{
		return copyBtn_;
	}
	fuse::ui::NodeHandle saveButton() const
	{
		return saveBtn_;
	}
	fuse::ui::NodeHandle clearButton() const
	{
		return clearBtn_;
	}
	fuse::ui::NodeHandle autoScrollCheck() const
	{
		return autoChk_;
	}
	fuse::ui::NodeHandle saveDialog() const
	{
		return saveDlg_;
	}

private:
	bool handleEvent(fuse::ui::EventContext& ec, const fuse::ui::Event& e);
	void startTimer();
	void insert(std::string_view decoded);
	void insertChunks(std::vector<model::ConsoleFeed::Chunk> chunks);

	fuse::ui::UiContext& ui_;
	PanelEventHandler handler_;
	fuse::ui::NodeHandle root_, copyBtn_, saveBtn_, clearBtn_, autoChk_, status_, saveDlg_;
	std::unique_ptr<CodeView> view_;
	model::ConsoleBuffer buffer_{50'000};
	std::shared_ptr<model::ConsoleFeed> feed_ = std::make_shared<model::ConsoleFeed>();
	fuse::ui::TimerHandle timer_;
	fuse::ui::TimerHandle rearm_;
	bool rearmPending_ = false;
	int flushHz_ = 60;
	int maxBytesPerFlush_ = 16 * 1024;
	bool autoScroll_ = true;
	std::string lastError_;
};

} // namespace retdec::gui::ui

#endif // RETDEC_GUI_UI_LIVE_CONSOLE_PANEL_H
