/**
 * @file include/retdec/gui/ui/progress_panel.h
 * @brief Progress panel: analysis progress dashboard (FuseUI port of the Qt
 *        ProgressPanel).
 *
 * Layout
 *   [summary: overall bar | N% | ƒ functions | instructions | elapsed]
 *   [stages: state icon (○ ◉ ✓ ✗) | name | progress bar | time]   (scrolls)
 *   [Stage Timeline: waterfall of stage start / duration bars]
 *   [Throughput: Functions/s | Instructions/s]
 *   [Export JSON]
 * The elapsed time ticks every second while an analysis runs and turns amber
 * at 80 % of the analysis budget, red past it (setAnalysisBudgetSecs). A
 * running stage without a percentage shows a moving (indeterminate) bar.
 * "Export JSON" copies the JSON to the clipboard, reports onExportRequested
 * and offers a save dialog.
 *
 * Worker threads report through model::AnalysisBridge; connectBridge() queues
 * those reports and replays them on the UI thread (UiContext::post), so they
 * take effect on the next UiContext::update().
 *
 * @copyright (c) 2026 Odin Loch trading as Imortek
 */

#ifndef RETDEC_GUI_UI_PROGRESS_PANEL_H
#define RETDEC_GUI_UI_PROGRESS_PANEL_H

#include "retdec/gui/model/progress_bridge.h"
#include "retdec/gui/model/progress_tracker.h"

#include <fuse/ui/ui.hpp>
#include <fuse/ui/widgets/widgets.hpp>

#include <cstdint>
#include <expected>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace retdec::gui::ui {

class ProgressPanel {
public:
	static constexpr std::string_view kTitle = "Progress";
	using StageState = model::StageState;

	ProgressPanel(fuse::ui::UiContext& ui, fuse::ui::NodeHandle parent);
	~ProgressPanel();
	ProgressPanel(const ProgressPanel&) = delete;
	ProgressPanel& operator=(const ProgressPanel&) = delete;

	fuse::ui::NodeHandle root() const
	{
		return root_;
	}
	std::string_view panelTitle() const
	{
		return kTitle;
	}

	/// Replays @p bridge's reports here (queued to the UI thread). One bridge
	/// at a time; nullptr disconnects. The UiContext must outlive the bridge's
	/// reporting threads' use of it.
	void connectBridge(model::AnalysisBridge* bridge);

	// ── direct update API (UI thread) ───────────────────────────────────────
	/// Unknown stages get a new row. Running with percent < 0 is indeterminate.
	void setStageState(std::string_view stageName, StageState state, int percent = -1);
	/// No-op for an unknown stage.
	void setStageElapsed(std::string_view stageName, int64_t ms);
	void setFunctionCount(int count);
	void setInstructionCount(int64_t count);
	void setThroughput(double fnPerSec, double instrPerSec);
	void setOverallProgress(int percent);
	/// Wall-time budget in seconds (Settings, max analysis time); 0 = none.
	void setAnalysisBudgetSecs(int secs);
	void resetAll();
	void clear()
	{
		resetAll();
	}

	// ── pipeline events (what the bridge delivers; callable directly) ───────
	void stageStarted(std::string_view stageName);
	void stageProgress(std::string_view stageName, int percent);
	void stageCompleted(std::string_view stageName, int64_t elapsedMs);
	void stageErrored(std::string_view stageName, std::string_view message);
	void analysisStarted();
	void analysisCompleted(int64_t totalMs);
	void analysisCancelled();

	// ── export ──────────────────────────────────────────────────────────────
	std::string exportJson() const
	{
		return tracker_.exportJson();
	}
	std::expected<void, std::string> exportJsonTo(const std::string& path) const;
	/// The Export JSON button: clipboard, onExportRequested, save dialog.
	void exportClicked();

	const model::ProgressTracker& tracker() const
	{
		return tracker_;
	}
	/// Clock for the elapsed time (tests); default steady_clock.
	void setClock(model::ProgressTracker::Clock clock)
	{
		tracker_.setClock(std::move(clock));
	}
	/// What the 1-second tick does (also called by the timer).
	void tick();

	// ── events out ──────────────────────────────────────────────────────────
	std::function<void(const std::string&)> onStageStarted;
	std::function<void(const std::string&, int64_t)> onStageCompleted;
	std::function<void(int64_t)> onAnalysisCompleted;
	std::function<void()> onExportRequested;
	/// Clipboard writer; unset = UiContext::setClipboardText.
	std::function<bool(const std::string&)> clipboardWriter;

	// ── test hooks ──────────────────────────────────────────────────────────
	struct StageRowNodes
	{
		fuse::ui::NodeHandle row, icon, name, bar, time;
	};
	/// Nodes of stage @p name (all invalid when unknown).
	StageRowNodes stageRow(std::string_view name) const;
	fuse::ui::NodeHandle overallBar() const
	{
		return overallBar_;
	}
	fuse::ui::NodeHandle percentLabel() const
	{
		return pctLabel_;
	}
	fuse::ui::NodeHandle functionCountLabel() const
	{
		return fnCountLabel_;
	}
	fuse::ui::NodeHandle instructionLabel() const
	{
		return instrLabel_;
	}
	fuse::ui::NodeHandle elapsedLabel() const
	{
		return elapsedLabel_;
	}
	fuse::ui::NodeHandle functionRateLabel() const
	{
		return fnRateLabel_;
	}
	fuse::ui::NodeHandle instructionRateLabel() const
	{
		return instrRateLabel_;
	}
	fuse::ui::NodeHandle waterfallNode() const
	{
		return waterfall_;
	}
	fuse::ui::NodeHandle exportButton() const
	{
		return exportBtn_;
	}
	fuse::ui::NodeHandle fileDialogNode() const
	{
		return fileDlg_;
	}
	/// Tooltip text of the elapsed label (budget warning), "" when none.
	const std::string& elapsedToolTip() const
	{
		return elapsedTip_;
	}
	model::BudgetLevel budgetState() const
	{
		return budget_;
	}
	/// Waterfall bars as last painted.
	std::vector<model::WaterfallBar> waterfallBars() const
	{
		return tracker_.waterfall();
	}

private:
	struct Row
	{
		std::string name;
		StageRowNodes nodes;
		fuse::ui::NodeHandle errorTip; ///< tooltip of the time column (error text)
	};
	struct Queue;

	void build(fuse::ui::NodeHandle parent);
	void addRowNodes(const std::string& name);
	void syncRows();
	void syncRow(std::size_t index);
	void syncSummary();
	void syncElapsed();
	void tooltip(fuse::ui::NodeHandle n, std::string_view text);
	void drainQueue();
	static void drainCommand(fuse::ui::UiContext& ui, fuse::ui::NodeHandle n, fuse::ui::u64 a, fuse::ui::u64 b);
	static void paintWaterfall(const fuse::ui::PaintContext& pc, fuse::ui::DrawList& dl);
	static fuse::ui::Size measureWaterfall(fuse::ui::UiContext& ui, fuse::ui::NodeHandle n, fuse::ui::Size avail);

	fuse::ui::UiContext& ui_;
	model::ProgressTracker tracker_;
	std::vector<Row> rows_;
	int budgetSecs_ = 0;
	model::BudgetLevel budget_ = model::BudgetLevel::Normal;
	std::string elapsedTip_;

	fuse::ui::NodeHandle root_, overallBar_, pctLabel_, fnCountLabel_, instrLabel_, elapsedLabel_;
	fuse::ui::NodeHandle stagesBox_, waterfall_, fnRateLabel_, instrRateLabel_, exportBtn_, fileDlg_;
	fuse::ui::TimerHandle tickTimer_;
	bool tickRunning_ = false;
	std::vector<fuse::ui::NodeHandle> tooltips_;
	fuse::ui::NodeHandle elapsedTipNode_;

	std::shared_ptr<Queue> queue_;
	model::AnalysisBridge::Subscription subscription_;
};

} // namespace retdec::gui::ui

#endif // RETDEC_GUI_UI_PROGRESS_PANEL_H
