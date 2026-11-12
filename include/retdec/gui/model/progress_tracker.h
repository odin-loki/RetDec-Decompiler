/**
 * @file include/retdec/gui/model/progress_tracker.h
 * @brief Analysis progress bookkeeping behind the Progress panel: stage rows
 *        and states, per-stage timing for the waterfall, overall percentage,
 *        counters, throughput, the time-budget warning and the JSON export.
 *
 * Port of the non-UI part of the Qt ProgressPanel. Not thread-safe: the panel
 * owns one on the UI thread; worker threads go through AnalysisBridge.
 *
 * @copyright (c) 2026 Odin Loch trading as Imortek
 */

#ifndef RETDEC_GUI_MODEL_PROGRESS_TRACKER_H
#define RETDEC_GUI_MODEL_PROGRESS_TRACKER_H

#include <forge/json/json.hpp>

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace retdec::gui::model {

enum class StageState
{
	Pending,
	Running,
	Done,
	Error
};

/// "pending" / "running" / "done" / "error".
std::string_view stageStateName(StageState s);
/// "○" / "◉" / "✓" / "✗".
std::string_view stageStateIcon(StageState s);

/// The fourteen pipeline stages the panel lists before any report arrives.
const std::vector<std::string>& defaultAnalysisStages();

/// "" (negative), "350ms", "2.5s", "1m30s" (the Qt formatMs).
std::string formatStageMs(int64_t ms);
/// "500 instr", "45K instr", "3M instr".
std::string formatInstructionCount(int64_t count);
/// Functions/s with one decimal ("3.5").
std::string formatFunctionRate(double fnPerSec);
/// Instructions/s: "1.5M" from a million up, else whole ("120").
std::string formatInstructionRate(double instrPerSec);
/// Waterfall bar colour of the stage at pipeline position @p index (0xRRGGBB).
uint32_t waterfallColor(int index);

/// Wall time against the analysis budget (Settings, max analysis time).
enum class BudgetLevel
{
	Normal,   ///< under 80 % (or no budget)
	Warning,  ///< 80 % and over
	Exceeded, ///< at or over the budget
};
BudgetLevel budgetLevel(int64_t elapsedMs, int budgetSecs);
/// The elapsed label's tooltip for a level ("" for Normal).
std::string budgetToolTip(BudgetLevel level, int budgetSecs);

struct StageTiming
{
	std::string name;
	int64_t startMs = -1;    ///< ms since analysis start, -1 = not started
	int64_t durationMs = -1; ///< -1 = not finished
};

struct StageRow
{
	std::string name;
	StageState state = StageState::Pending;
	int percent = 0;        ///< 0-100; -1 = running, amount unknown (indeterminate bar)
	int64_t elapsedMs = -1; ///< -1 = not reported
	std::string message;    ///< error text (tooltip of the time column)
};

/// One bar of the stage timeline.
struct WaterfallBar
{
	std::string label; ///< stage name, first 18 characters
	int stageIndex = 0;
	int64_t startMs = 0;
	int64_t durationMs = 0;
	uint32_t color = 0;
};

class ProgressTracker {
public:
	/// Monotonic milliseconds (default: std::chrono::steady_clock).
	using Clock = std::function<int64_t()>;

	ProgressTracker();
	void setClock(Clock clock);

	const std::vector<StageRow>& stages() const
	{
		return rows_;
	}
	const std::vector<StageTiming>& timings() const
	{
		return timings_;
	}
	int stageIndex(std::string_view name) const;
	/// Index of @p name, appending a pending stage when it is new.
	int ensureStage(std::string_view name);

	// ── direct updates (the Qt setStage* API) ───────────────────────────────
	/// Unknown stages are added. Running with percent < 0 is indeterminate;
	/// Done is 100 %; Error keeps a positive percent. Recomputes overall.
	void setStageState(std::string_view name, StageState state, int percent = -1);
	/// False (and nothing changes) for an unknown stage.
	bool setStageElapsed(std::string_view name, int64_t ms);
	void setFunctionCount(int count)
	{
		functionCount_ = count;
	}
	void setInstructionCount(int64_t count)
	{
		instrCount_ = count;
	}
	void setThroughput(double fnPerSec, double instrPerSec);
	/// Overrides the overall percentage (clamped 0-100) until the next stage change.
	void setOverallProgress(int percent);

	// ── pipeline events (what the bridge delivers) ──────────────────────────
	void stageStarted(std::string_view name);
	void stageProgress(std::string_view name, int percent);
	void stageCompleted(std::string_view name, int64_t elapsedMs);
	void stageErrored(std::string_view name, std::string_view message);
	void analysisStarted();
	void analysisCompleted(int64_t totalMs);
	/// Stages still running go back to pending.
	void analysisCancelled();

	/// Everything back to the default stages, all pending, counters zero.
	void reset();

	int overallPercent() const
	{
		return overall_;
	}
	int functionCount() const
	{
		return functionCount_;
	}
	int64_t instructionCount() const
	{
		return instrCount_;
	}
	std::optional<double> functionRate() const
	{
		return fnRate_;
	}
	std::optional<double> instructionRate() const
	{
		return instrRate_;
	}
	bool running() const
	{
		return running_;
	}
	bool started() const
	{
		return started_;
	}
	/// Wall time since analysisStarted() (frozen at the reported total once
	/// completed; 0 before any start).
	int64_t elapsedMs() const;
	/// Done or failed stages.
	int finishedStages() const;

	/// Bars for every stage that has a start and a positive duration.
	std::vector<WaterfallBar> waterfall() const;
	int64_t waterfallEnd() const;

	/// {elapsed_ms, function_count, instruction_count, stages[{name, state,
	/// elapsed_ms}], timeline[{name, start_ms, duration_ms}]}.
	forge::json::value exportJsonValue() const;
	/// exportJsonValue() as indented text (QJsonDocument::Indented layout).
	std::string exportJson() const;

private:
	void recomputeOverall();
	StageTiming* timing(std::string_view name);

	Clock clock_;
	std::vector<StageRow> rows_;
	std::vector<StageTiming> timings_;
	int overall_ = 0;
	int functionCount_ = 0;
	int64_t instrCount_ = 0;
	std::optional<double> fnRate_, instrRate_;
	bool running_ = false;
	bool started_ = false;
	int64_t startMs_ = 0;
	int64_t finalMs_ = -1;
};

} // namespace retdec::gui::model

#endif // RETDEC_GUI_MODEL_PROGRESS_TRACKER_H
