/**
 * @file include/retdec/gui/ui/inspect_panel.h
 * @brief InspectPanel — runs retdec-fileinfo (--json --verbose) and optionally
 *        retdec-unpacker on the loaded binary and shows the result.
 *
 * Row 1: status, Refresh fileinfo, Decompile mode, Copy hashes.
 * Row 2: Unpack output path, Brute, Open when done, Run unpacker, Open unpacked.
 * Tabs: Summary (hashes + root scalar fields) / JSON (pretty-printed, capped).
 *
 * The processes run in model::InspectModel (Forge, worker threads); the panel
 * polls it from a UI timer while something runs and reports through callbacks.
 *
 * @copyright (c) 2026 Odin Loch trading as Imortek
 */

#ifndef RETDEC_GUI_UI_INSPECT_PANEL_H
#define RETDEC_GUI_UI_INSPECT_PANEL_H

#include "retdec/gui/model/inspect_model.h"
#include "retdec/gui/ui/empty_state.h"
#include "retdec/gui/ui/tri_pane_code_view.h"

#include <fuse/ui/ui.hpp>
#include <fuse/ui/widgets/widgets.hpp>

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace retdec::gui::ui {

class InspectPanel {
public:
	InspectPanel(fuse::ui::UiContext& ui, fuse::ui::NodeHandle parent);
	~InspectPanel();
	InspectPanel(const InspectPanel&) = delete;
	InspectPanel& operator=(const InspectPanel&) = delete;

	fuse::ui::NodeHandle root() const { return root_; }
	std::string panelTitle() const { return "Inspect"; }

	// ── Commands ─────────────────────────────────────────────────────────────
	/// Runs fileinfo on @p binaryPath (an empty @p fileinfoExe reports "not
	/// found"; an empty path only resets the buttons).
	void runFileinfo(std::string_view binaryPath, std::string_view fileinfoExe);
	/// Executable used by "Run unpacker" (resolved by the app).
	void setUnpackerExecutable(std::string_view exe) { unpackerExe_ = std::string(exe); }
	void refresh();
	void runUnpacker();
	void openUnpacked();
	void copyHashes();
	void setUnpackOutput(std::string_view path);
	void setBrute(bool on);
	void setAutoOpen(bool on);
	void clear();
	/// Collects finished runs (the UI timer calls this while busy).
	void poll();
	bool busy() const { return model_.anyRunning(); }

	/// Last successfully parsed fileinfo result, if any.
	const std::optional<model::FileinfoInfo>& fileinfo() const { return info_; }
	/// Raw stdout of the last successful fileinfo run.
	const std::string& fileinfoJson() const { return lastJson_; }

	// ── Events out ───────────────────────────────────────────────────────────
	std::function<void()> onRequestDecompileMode;
	/// Load a different binary (e.g. the unpacked output).
	std::function<void(const std::string& absolutePath)> onRequestOpenBinary;
	/// A tool finished (for the command log).
	std::function<void(const std::string& tool, const std::vector<std::string>& args, const std::string& cwd, int exitCode,
	                   int64_t elapsedMs, const std::string& outputTail)>
		onCliToolFinished;
	/// A tool is starting (the console's banner).
	std::function<void(const std::string& label, const std::vector<std::string>& args, const std::string& cwd)> onProcessStarted;
	/// Output of a finished tool (for the live console).
	std::function<void(const std::string& label, const std::string& out, const std::string& err)> onProcessOutput;
	/// Parsed fileinfo for @p absPath.
	std::function<void(const model::FileinfoInfo& info, const std::string& json, const std::string& absPath)> onFileinfoReady;

	// ── Test hooks ───────────────────────────────────────────────────────────
	std::string statusText() const;
	int summaryRowCount() const { return int(summary_.rows()); }
	std::string summaryCell(int row, int col) const;
	std::string rawText() const { return raw_->text(); }
	std::string unpackOutput() const;
	model::InspectModel& model() { return model_; }
	fuse::ui::NodeHandle refreshButton() const { return refreshBtn_; }
	fuse::ui::NodeHandle decompileModeButton() const { return decompileBtn_; }
	fuse::ui::NodeHandle copyHashesButton() const { return copyHashesBtn_; }
	fuse::ui::NodeHandle unpackButton() const { return unpackBtn_; }
	fuse::ui::NodeHandle openUnpackedButton() const { return openUnpackedBtn_; }
	fuse::ui::NodeHandle unpackOutputEdit() const { return unpackOut_; }
	fuse::ui::NodeHandle bruteCheck() const { return bruteChk_; }
	fuse::ui::NodeHandle autoOpenCheck() const { return autoOpenChk_; }
	fuse::ui::NodeHandle tabs() const { return tabs_; }
	int currentTab() const;

private:
	void setSummaryMessage(std::string_view msg);
	void setRawCapped(std::string_view text, std::size_t cap);
	void updateButtonStates();
	void handleRun(const model::ToolRun& run);
	void handleFileinfo(const model::ToolRun& run);
	void handleUnpacker(const model::ToolRun& run);
	void ensurePolling();
	void status(std::string_view s);

	fuse::ui::UiContext& ui_;
	PanelEventHandler handler_;
	fuse::ui::NodeHandle root_, status_, refreshBtn_, decompileBtn_, copyHashesBtn_;
	fuse::ui::NodeHandle unpackOut_, bruteChk_, autoOpenChk_, unpackBtn_, openUnpackedBtn_, tabs_, summaryTable_;
	fuse::ui::widgets::StringTableModel summary_{"Field\tValue"};
	std::unique_ptr<CodeView> raw_;
	model::InspectModel model_;
	fuse::ui::TimerHandle pollTimer_;
	bool polling_ = false;
	std::string binaryPath_, fileinfoExe_, unpackerExe_, lastJson_, lastFileinfoBinary_;
	std::optional<model::FileinfoInfo> info_;
};

} // namespace retdec::gui::ui

#endif // RETDEC_GUI_UI_INSPECT_PANEL_H
