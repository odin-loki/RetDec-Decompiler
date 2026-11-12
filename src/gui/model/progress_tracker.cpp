/**
 * @file src/gui/model/progress_tracker.cpp
 * @brief Analysis progress bookkeeping.
 * @copyright (c) 2026 Odin Loch trading as Imortek
 */

#include "retdec/gui/model/progress_tracker.h"

#include "retdec/gui/model/funclist_export.h"
#include "retdec/gui/model/strings_model.h"

#include <algorithm>
#include <chrono>
#include <format>
#include <iterator>

namespace retdec::gui::model {

std::string_view stageStateName(StageState s)
{
	switch (s)
	{
	case StageState::Pending: return "pending";
	case StageState::Running: return "running";
	case StageState::Done: return "done";
	case StageState::Error: return "error";
	}
	return "unknown";
}

std::string_view stageStateIcon(StageState s)
{
	switch (s)
	{
	case StageState::Pending: return "\xE2\x97\x8B"; // ○
	case StageState::Running: return "\xE2\x97\x89"; // ◉
	case StageState::Done: return "\xE2\x9C\x93";    // ✓
	case StageState::Error: return "\xE2\x9C\x97";   // ✗
	}
	return "?";
}

const std::vector<std::string>& defaultAnalysisStages()
{
	static const std::vector<std::string> kStages = {
		"Binary loading",
		"RTTI reconstruction",
		"String detection",
		"SSA construction",
		"Variable recovery",
		"Alias analysis",
		"Type inference",
		"CFG structuring",
		"Calling convention",
		"Dead code elimination",
		"Inter-procedural analysis",
		"Code generation",
		"STL recovery",
		"Pattern / crypto detection",
	};
	return kStages;
}

std::string formatStageMs(int64_t ms)
{
	if (ms < 0) return "";
	if (ms < 1000) return std::format("{}ms", ms);
	if (ms < 60'000) return std::format("{}.{}s", ms / 1000, (ms % 1000) / 100);
	return std::format("{}m{}s", ms / 60'000, (ms % 60'000) / 1000);
}

std::string formatInstructionCount(int64_t count)
{
	if (count < 1000) return std::format("{} instr", count);
	if (count < 1'000'000) return std::format("{}K instr", count / 1000);
	return std::format("{}M instr", count / 1'000'000);
}

std::string formatFunctionRate(double fnPerSec)
{
	return std::format("{:.1f}", fnPerSec);
}

std::string formatInstructionRate(double instrPerSec)
{
	return instrPerSec >= 1e6 ? std::format("{:.1f}M", instrPerSec / 1e6) : std::format("{:.0f}", instrPerSec);
}

uint32_t waterfallColor(int index)
{
	static constexpr uint32_t kPalette[] = {
		0x89b4fa, // blue
		0xcba6f7, // mauve
		0xa6e3a1, // green
		0xfab387, // peach
		0xf9e2af, // yellow
		0x89dceb, // sky
		0xf5c2e7, // pink
		0x74c7ec, // sapphire
		0x94e2d5, // teal
		0xa6e3a1, // green
		0x89b4fa,
		0xcba6f7,
		0xa6e3a1,
		0xfab387,
	};
	const int n = static_cast<int>(std::size(kPalette));
	return kPalette[((index % n) + n) % n];
}

BudgetLevel budgetLevel(int64_t elapsedMs, int budgetSecs)
{
	if (budgetSecs <= 0) return BudgetLevel::Normal;
	const int64_t budgetMs = static_cast<int64_t>(budgetSecs) * 1000;
	if (elapsedMs >= budgetMs) return BudgetLevel::Exceeded;
	if (elapsedMs >= (budgetMs * 80) / 100) return BudgetLevel::Warning;
	return BudgetLevel::Normal;
}

std::string budgetToolTip(BudgetLevel level, int budgetSecs)
{
	switch (level)
	{
	case BudgetLevel::Exceeded:
		return std::format(
			"Wall time exceeded the {}s analysis budget; decompile still running; press F6 to cancel.", budgetSecs);
	case BudgetLevel::Warning: return std::format("Approaching the {}s analysis budget.", budgetSecs);
	default: return {};
	}
}

// ─── ProgressTracker ─────────────────────────────────────────────────────────

ProgressTracker::ProgressTracker()
{
	clock_ = [] {
		return static_cast<int64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
										std::chrono::steady_clock::now().time_since_epoch())
										.count());
	};
	reset();
}

void ProgressTracker::setClock(Clock clock)
{
	if (clock) clock_ = std::move(clock);
}

int ProgressTracker::stageIndex(std::string_view name) const
{
	for (std::size_t i = 0; i < rows_.size(); ++i)
		if (rows_[i].name == name) return static_cast<int>(i);
	return -1;
}

int ProgressTracker::ensureStage(std::string_view name)
{
	const int i = stageIndex(name);
	if (i >= 0) return i;
	StageRow r;
	r.name = std::string(name);
	rows_.push_back(std::move(r));
	timings_.push_back(StageTiming{std::string(name), -1, -1});
	return static_cast<int>(rows_.size()) - 1;
}

StageTiming* ProgressTracker::timing(std::string_view name)
{
	for (auto& t: timings_)
		if (t.name == name) return &t;
	return nullptr;
}

void ProgressTracker::setStageState(std::string_view name, StageState state, int percent)
{
	auto& r = rows_[static_cast<std::size_t>(ensureStage(name))];
	r.state = state;
	switch (state)
	{
	case StageState::Pending: r.percent = 0; break;
	case StageState::Running: r.percent = percent < 0 ? -1 : std::clamp(percent, 0, 100); break;
	case StageState::Done: r.percent = 100; break;
	case StageState::Error: r.percent = percent > 0 ? std::min(percent, 100) : 0; break;
	}
	recomputeOverall();
}

bool ProgressTracker::setStageElapsed(std::string_view name, int64_t ms)
{
	const int i = stageIndex(name);
	if (i < 0) return false;
	rows_[static_cast<std::size_t>(i)].elapsedMs = ms;
	if (StageTiming* t = timing(name)) t->durationMs = ms;
	return true;
}

void ProgressTracker::setThroughput(double fnPerSec, double instrPerSec)
{
	fnRate_ = fnPerSec;
	instrRate_ = instrPerSec;
}

void ProgressTracker::setOverallProgress(int percent)
{
	overall_ = std::clamp(percent, 0, 100);
}

void ProgressTracker::stageStarted(std::string_view name)
{
	setStageState(name, StageState::Running, -1);
	if (StageTiming* t = timing(name))
	{
		t->startMs = running_ ? elapsedMs() : 0;
		t->durationMs = -1;
	}
}

void ProgressTracker::stageProgress(std::string_view name, int percent)
{
	setStageState(name, StageState::Running, percent);
}

void ProgressTracker::stageCompleted(std::string_view name, int64_t elapsed)
{
	setStageState(name, StageState::Done);
	setStageElapsed(name, elapsed);
}

void ProgressTracker::stageErrored(std::string_view name, std::string_view message)
{
	setStageState(name, StageState::Error);
	rows_[static_cast<std::size_t>(stageIndex(name))].message = std::string(message);
}

void ProgressTracker::analysisStarted()
{
	running_ = true;
	started_ = true;
	startMs_ = clock_();
	finalMs_ = -1;
}

void ProgressTracker::analysisCompleted(int64_t totalMs)
{
	running_ = false;
	finalMs_ = totalMs;
	overall_ = 100;
}

void ProgressTracker::analysisCancelled()
{
	if (running_) finalMs_ = clock_() - startMs_;
	running_ = false;
	for (auto& r: rows_)
		if (r.state == StageState::Running) setStageState(r.name, StageState::Pending);
}

void ProgressTracker::reset()
{
	rows_.clear();
	timings_.clear();
	for (const auto& name: defaultAnalysisStages())
		ensureStage(name);
	overall_ = 0;
	functionCount_ = 0;
	instrCount_ = 0;
	fnRate_.reset();
	instrRate_.reset();
	running_ = false;
	started_ = false;
	startMs_ = 0;
	finalMs_ = -1;
}

int64_t ProgressTracker::elapsedMs() const
{
	if (running_) return clock_() - startMs_;
	if (finalMs_ >= 0) return finalMs_;
	return 0;
}

int ProgressTracker::finishedStages() const
{
	int done = 0;
	for (const auto& r: rows_)
		if (r.state == StageState::Done || r.state == StageState::Error) ++done;
	return done;
}

void ProgressTracker::recomputeOverall()
{
	if (rows_.empty()) return;
	overall_ = finishedStages() * 100 / static_cast<int>(rows_.size());
}

std::vector<WaterfallBar> ProgressTracker::waterfall() const
{
	std::vector<WaterfallBar> bars;
	for (std::size_t i = 0; i < timings_.size(); ++i)
	{
		const auto& t = timings_[i];
		if (t.startMs < 0 || t.durationMs <= 0) continue;
		WaterfallBar b;
		b.label = utf8Left(t.name, 18);
		b.stageIndex = static_cast<int>(i);
		b.startMs = t.startMs;
		b.durationMs = t.durationMs;
		b.color = waterfallColor(static_cast<int>(i));
		bars.push_back(std::move(b));
	}
	return bars;
}

int64_t ProgressTracker::waterfallEnd() const
{
	int64_t maxEnd = 0;
	for (const auto& t: timings_)
		if (t.startMs >= 0 && t.durationMs > 0) maxEnd = std::max(maxEnd, t.startMs + t.durationMs);
	return maxEnd;
}

forge::json::value ProgressTracker::exportJsonValue() const
{
	using forge::json::array;
	using forge::json::object;
	array stages;
	for (const auto& r: rows_)
	{
		stages.emplace_back(object{
			{"name", r.name},
			{"state", std::string(stageStateName(r.state))},
			{"elapsed_ms", static_cast<long long>(r.elapsedMs)},
		});
	}
	array timeline;
	for (const auto& t: timings_)
	{
		if (t.startMs < 0) continue;
		timeline.emplace_back(object{
			{"name", t.name},
			{"start_ms", static_cast<long long>(t.startMs)},
			{"duration_ms", static_cast<long long>(t.durationMs)},
		});
	}
	return object{
		{"elapsed_ms", static_cast<long long>(started_ ? elapsedMs() : 0)},
		{"function_count", functionCount_},
		{"instruction_count", static_cast<long long>(instrCount_)},
		{"stages", std::move(stages)},
		{"timeline", std::move(timeline)},
	};
}

std::string ProgressTracker::exportJson() const
{
	return funclistIndentedJson(exportJsonValue());
}

} // namespace retdec::gui::model
