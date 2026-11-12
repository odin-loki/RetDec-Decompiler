/**
 * @file tests/gui/progress_panel_test.cpp
 * @brief Progress: the Qt ProgressPanel / AnalysisBridge tests ported to the
 *        FuseUI panel and model::AnalysisBridge, plus tracker and headless UI
 *        tests.
 * @copyright (c) 2026 Odin Loch trading as Imortek
 */

#include "retdec/gui/model/progress_bridge.h"
#include "retdec/gui/model/progress_tracker.h"
#include "retdec/gui/ui/progress_panel.h"

#include "table_panels_test_util.h"

#include <forge/json/json.hpp>

#include <gtest/gtest.h>

#include <atomic>
#include <filesystem>
#include <memory>
#include <thread>

using retdec::gui::model::AnalysisBridge;
using retdec::gui::model::BudgetLevel;
using retdec::gui::model::ProgressTracker;
using retdec::gui::model::StageState;
using retdec::gui::ui::ProgressPanel;
using retdec_gui_tp_test::Headless;
namespace model = retdec::gui::model;
namespace w = fuse::ui::widgets;

namespace {

forge::json::value parseJson(const std::string& s)
{
	return forge::json::parse(s);
}

} // namespace

// ─── Ported: ProgressPanel ───────────────────────────────────────────────────

class ProgressPanelTest : public ::testing::Test {
protected:
	void SetUp() override
	{
		panel = std::make_unique<ProgressPanel>(h.ui, h.ui.root());
	}
	void TearDown() override
	{
		h.frame();
	}
	Headless h;
	std::unique_ptr<ProgressPanel> panel;
};

TEST_F(ProgressPanelTest, ConstructsWithoutCrash)
{
	EXPECT_NE(panel.get(), nullptr);
}

TEST_F(ProgressPanelTest, PanelTitleIsProgress)
{
	EXPECT_EQ(panel->panelTitle(), "Progress");
}

TEST_F(ProgressPanelTest, SetStagePending)
{
	EXPECT_NO_THROW(panel->setStageState("Binary loading", StageState::Pending));
	EXPECT_EQ(h.ui.text(panel->stageRow("Binary loading").icon), "\xE2\x97\x8B");
}

TEST_F(ProgressPanelTest, SetStageRunningIndeterminate)
{
	EXPECT_NO_THROW(panel->setStageState("Binary loading", StageState::Running, -1));
	EXPECT_EQ(panel->tracker().stages()[0].percent, -1);
	EXPECT_EQ(h.ui.text(panel->stageRow("Binary loading").icon), "\xE2\x97\x89");
	EXPECT_TRUE(fuse::ui::widgets::progressIndeterminate(h.ui, panel->stageRow("Binary loading").bar));
	panel->setStageState("Binary loading", StageState::Running, 30);
	EXPECT_FALSE(fuse::ui::widgets::progressIndeterminate(h.ui, panel->stageRow("Binary loading").bar));
	EXPECT_FLOAT_EQ(h.ui.value(panel->stageRow("Binary loading").bar), 0.3f);
}

TEST_F(ProgressPanelTest, SetStageRunningWithPercent)
{
	EXPECT_NO_THROW(panel->setStageState("SSA construction", StageState::Running, 42));
	EXPECT_FLOAT_EQ(h.ui.value(panel->stageRow("SSA construction").bar), 0.42f);
}

TEST_F(ProgressPanelTest, SetStageDone)
{
	EXPECT_NO_THROW(panel->setStageState("Binary loading", StageState::Done));
	EXPECT_EQ(h.ui.text(panel->stageRow("Binary loading").icon), "\xE2\x9C\x93");
	EXPECT_FLOAT_EQ(h.ui.value(panel->stageRow("Binary loading").bar), 1.f);
}

TEST_F(ProgressPanelTest, SetStageError)
{
	EXPECT_NO_THROW(panel->setStageState("Type inference", StageState::Error, 25));
	EXPECT_EQ(h.ui.text(panel->stageRow("Type inference").icon), "\xE2\x9C\x97");
	EXPECT_FLOAT_EQ(h.ui.value(panel->stageRow("Type inference").bar), 0.25f);
}

TEST_F(ProgressPanelTest, SetStateForUnknownStageCreatesRow)
{
	EXPECT_NO_THROW(panel->setStageState("Custom stage", StageState::Running, 50));
	EXPECT_TRUE(h.ui.alive(panel->stageRow("Custom stage").row));
	EXPECT_EQ(panel->tracker().stages().size(), 15u);
}

TEST_F(ProgressPanelTest, SetElapsedMilliseconds)
{
	EXPECT_NO_THROW(panel->setStageElapsed("Binary loading", 350));
	EXPECT_EQ(h.ui.text(panel->stageRow("Binary loading").time), "350ms");
}

TEST_F(ProgressPanelTest, SetElapsedSeconds)
{
	EXPECT_NO_THROW(panel->setStageElapsed("SSA construction", 2500));
	EXPECT_EQ(h.ui.text(panel->stageRow("SSA construction").time), "2.5s");
}

TEST_F(ProgressPanelTest, SetElapsedMinutes)
{
	EXPECT_NO_THROW(panel->setStageElapsed("Inter-procedural analysis", 90000));
	EXPECT_EQ(h.ui.text(panel->stageRow("Inter-procedural analysis").time), "1m30s");
}

TEST_F(ProgressPanelTest, SetElapsedForUnknownStageIsNoop)
{
	EXPECT_NO_THROW(panel->setStageElapsed("Does not exist", 1000));
	EXPECT_EQ(panel->tracker().stages().size(), 14u);
}

TEST_F(ProgressPanelTest, OverallProgressUpdatesAfterDoneStages)
{
	panel->setStageState("Binary loading", StageState::Done);
	panel->setStageState("RTTI reconstruction", StageState::Done);
	// 2 out of 14 default stages = 14 %
	EXPECT_EQ(panel->tracker().overallPercent(), 14);
	EXPECT_EQ(h.ui.text(panel->percentLabel()), "14%");
	EXPECT_NEAR(h.ui.value(panel->overallBar()), 0.14f, 1e-5f);
}

TEST_F(ProgressPanelTest, SetOverallProgressDirectly)
{
	EXPECT_NO_THROW(panel->setOverallProgress(75));
	EXPECT_EQ(h.ui.text(panel->percentLabel()), "75%");
}

TEST_F(ProgressPanelTest, SetFunctionCount)
{
	EXPECT_NO_THROW(panel->setFunctionCount(1234));
	EXPECT_EQ(h.ui.text(panel->functionCountLabel()), "1234 fn");
}

TEST_F(ProgressPanelTest, SetInstructionCountSmall)
{
	EXPECT_NO_THROW(panel->setInstructionCount(500));
	EXPECT_EQ(h.ui.text(panel->instructionLabel()), "500 instr");
}

TEST_F(ProgressPanelTest, SetInstructionCountKilo)
{
	EXPECT_NO_THROW(panel->setInstructionCount(45'000));
	EXPECT_EQ(h.ui.text(panel->instructionLabel()), "45K instr");
}

TEST_F(ProgressPanelTest, SetInstructionCountMega)
{
	EXPECT_NO_THROW(panel->setInstructionCount(3'200'000));
	EXPECT_EQ(h.ui.text(panel->instructionLabel()), "3M instr");
}

TEST_F(ProgressPanelTest, SetThroughputLow)
{
	EXPECT_NO_THROW(panel->setThroughput(3.5, 120.0));
	EXPECT_EQ(h.ui.text(panel->functionRateLabel()), "3.5");
	EXPECT_EQ(h.ui.text(panel->instructionRateLabel()), "120");
}

TEST_F(ProgressPanelTest, SetThroughputHighInstructions)
{
	EXPECT_NO_THROW(panel->setThroughput(120.0, 1'500'000.0));
	EXPECT_EQ(h.ui.text(panel->instructionRateLabel()), "1.5M");
}

TEST_F(ProgressPanelTest, ResetAllRestoresState)
{
	panel->setStageState("Binary loading", StageState::Done);
	panel->setStageState("Custom", StageState::Done);
	panel->setFunctionCount(9999);
	panel->setThroughput(10.0, 1000.0);
	EXPECT_NO_THROW(panel->resetAll());
	EXPECT_EQ(h.ui.text(panel->functionCountLabel()), "0 fn");
	EXPECT_EQ(h.ui.text(panel->functionRateLabel()), "\xE2\x80\x94");
	EXPECT_EQ(h.ui.text(panel->percentLabel()), "0%");
	EXPECT_EQ(panel->tracker().stages().size(), 14u);
	EXPECT_FALSE(h.ui.alive(panel->stageRow("Custom").row));
	EXPECT_EQ(h.ui.text(panel->stageRow("Binary loading").icon), "\xE2\x97\x8B");
}

TEST_F(ProgressPanelTest, ClearCallsResetAll)
{
	panel->setStageState("Code generation", StageState::Running, 30);
	EXPECT_NO_THROW(panel->clear());
	EXPECT_EQ(panel->tracker().stages()[11].state, StageState::Pending);
}

TEST_F(ProgressPanelTest, ExportJsonIsValidJson)
{
	EXPECT_NO_THROW(parseJson(panel->exportJson()));
}

TEST_F(ProgressPanelTest, ExportJsonHasRequiredTopLevelKeys)
{
	const auto root = parseJson(panel->exportJson());
	EXPECT_NE(root.find("elapsed_ms"), nullptr);
	EXPECT_NE(root.find("function_count"), nullptr);
	EXPECT_NE(root.find("instruction_count"), nullptr);
	EXPECT_NE(root.find("stages"), nullptr);
	EXPECT_NE(root.find("timeline"), nullptr);
}

TEST_F(ProgressPanelTest, ExportJsonStagesArrayHasDefaultStages)
{
	EXPECT_EQ(parseJson(panel->exportJson())["stages"].arr().size(), 14u);
}

TEST_F(ProgressPanelTest, ExportJsonStageHasStateField)
{
	const auto root = parseJson(panel->exportJson());
	const auto& first = root["stages"][std::size_t{0}];
	EXPECT_NE(first.find("name"), nullptr);
	EXPECT_NE(first.find("state"), nullptr);
	EXPECT_EQ(first["state"].str(), "pending");
}

TEST_F(ProgressPanelTest, ExportJsonReflectsDoneState)
{
	panel->setStageState("Binary loading", StageState::Done);
	EXPECT_EQ(parseJson(panel->exportJson())["stages"][std::size_t{0}]["state"].str(), "done");
}

TEST_F(ProgressPanelTest, ExportJsonFunctionCountMatchesSet)
{
	panel->setFunctionCount(42);
	EXPECT_EQ(parseJson(panel->exportJson())["function_count"].i(), 42);
}

// ─── Ported: AnalysisBridge signal emission ──────────────────────────────────

class ProgressBridgeTest : public ::testing::Test {
protected:
	void SetUp() override
	{
		bridge = std::make_unique<AnalysisBridge>();
		AnalysisBridge::Listener l;
		l.stageStarted = [this](const std::string& s) { started.push_back(s); };
		l.progressUpdated = [this](const std::string&, int p) { percents.push_back(p); };
		l.stageCompleted = [this](const std::string&, int64_t ms) { completed.push_back(ms); };
		l.stageErrored = [this](const std::string&, const std::string& m) { errors.push_back(m); };
		l.functionCountChanged = [this](int c) { counts.push_back(c); };
		l.instructionCountChanged = [this](int64_t c) { instrs.push_back(c); };
		l.throughputChanged = [this](double f, double) { rates.push_back(f); };
		l.analysisStarted = [this] { ++analysisStarted; };
		l.analysisCompleted = [this](int64_t ms) { totals.push_back(ms); };
		l.analysisCancelled = [this] { ++cancelled; };
		sub = bridge->subscribe(std::move(l));
	}
	std::unique_ptr<AnalysisBridge> bridge;
	AnalysisBridge::Subscription sub;
	std::vector<std::string> started, errors;
	std::vector<int> percents, counts;
	std::vector<int64_t> completed, instrs, totals;
	std::vector<double> rates;
	int analysisStarted = 0, cancelled = 0;
};

TEST_F(ProgressBridgeTest, ConstructsWithoutCrash)
{
	EXPECT_NE(bridge.get(), nullptr);
	EXPECT_EQ(bridge->listenerCount(), 1u);
}

TEST_F(ProgressBridgeTest, ReportStageStartedEmitsSignal)
{
	bridge->reportStageStarted("Binary loading");
	ASSERT_EQ(started.size(), 1u);
	EXPECT_EQ(started[0], "Binary loading");
}

TEST_F(ProgressBridgeTest, ReportProgressEmitsSignal)
{
	bridge->reportProgress("SSA construction", 55);
	ASSERT_EQ(percents.size(), 1u);
	EXPECT_EQ(percents[0], 55);
}

TEST_F(ProgressBridgeTest, ReportStageCompletedEmitsSignal)
{
	bridge->reportStageCompleted("Binary loading", 350);
	ASSERT_EQ(completed.size(), 1u);
	EXPECT_EQ(completed[0], 350);
}

TEST_F(ProgressBridgeTest, ReportStageErrorEmitsSignal)
{
	bridge->reportStageError("Type inference", "Out of memory");
	ASSERT_EQ(errors.size(), 1u);
	EXPECT_EQ(errors[0], "Out of memory");
}

TEST_F(ProgressBridgeTest, ReportFunctionCountEmitsSignal)
{
	bridge->reportFunctionCount(777);
	ASSERT_EQ(counts.size(), 1u);
	EXPECT_EQ(counts[0], 777);
}

TEST_F(ProgressBridgeTest, ReportInstructionCountEmitsSignal)
{
	bridge->reportInstructionCount(1'234'567LL);
	ASSERT_EQ(instrs.size(), 1u);
	EXPECT_EQ(instrs[0], 1'234'567LL);
}

TEST_F(ProgressBridgeTest, ReportThroughputEmitsSignal)
{
	bridge->reportThroughput(12.5, 500000.0);
	ASSERT_EQ(rates.size(), 1u);
	EXPECT_DOUBLE_EQ(rates[0], 12.5);
}

TEST_F(ProgressBridgeTest, ReportAnalysisStartedEmitsSignal)
{
	bridge->reportAnalysisStarted();
	EXPECT_EQ(analysisStarted, 1);
}

TEST_F(ProgressBridgeTest, ReportAnalysisCompletedEmitsSignal)
{
	bridge->reportAnalysisCompleted(12'345LL);
	ASSERT_EQ(totals.size(), 1u);
	EXPECT_EQ(totals[0], 12'345LL);
}

TEST_F(ProgressBridgeTest, ReportAnalysisCancelledEmitsSignal)
{
	bridge->reportAnalysisCancelled();
	EXPECT_EQ(cancelled, 1);
}

TEST_F(ProgressBridgeTest, SubscriptionLifetime)
{
	sub.reset();
	EXPECT_EQ(bridge->listenerCount(), 0u);
	bridge->reportFunctionCount(1);
	EXPECT_TRUE(counts.empty());
	auto s2 = bridge->subscribe({});
	EXPECT_TRUE(s2.active());
	bridge.reset(); // the subscription may outlive the bridge
	EXPECT_FALSE(s2.active());
	s2.reset();
}

// ─── Ported: connectBridge integration ───────────────────────────────────────

class ProgressBridgeIntegrationTest : public ::testing::Test {
protected:
	void SetUp() override
	{
		panel = std::make_unique<ProgressPanel>(h.ui, h.ui.root());
		bridge = std::make_unique<AnalysisBridge>();
		panel->connectBridge(bridge.get());
	}
	Headless h;
	std::unique_ptr<ProgressPanel> panel;
	std::unique_ptr<AnalysisBridge> bridge;
};

TEST_F(ProgressBridgeIntegrationTest, BridgeStartedTriggersPanelSignal)
{
	int count = 0;
	panel->onStageStarted = [&](const std::string&) { ++count; };
	bridge->reportStageStarted("Binary loading");
	EXPECT_EQ(count, 0); // queued for the UI thread
	h.frame();
	EXPECT_GE(count, 1);
}

TEST_F(ProgressBridgeIntegrationTest, BridgeCompletedTriggersPanelSignal)
{
	std::vector<int64_t> ms;
	panel->onStageCompleted = [&](const std::string&, int64_t v) { ms.push_back(v); };
	bridge->reportStageCompleted("Binary loading", 250);
	h.frame();
	ASSERT_GE(ms.size(), 1u);
	EXPECT_EQ(ms[0], 250);
}

TEST_F(ProgressBridgeIntegrationTest, BridgeAnalysisCompletedTriggersPanelSignal)
{
	int count = 0;
	panel->onAnalysisCompleted = [&](int64_t) { ++count; };
	bridge->reportAnalysisCompleted(9000LL);
	h.frame();
	EXPECT_GE(count, 1);
}

TEST_F(ProgressBridgeIntegrationTest, BridgeProgressUpdatesDoNotCrash)
{
	bridge->reportAnalysisStarted();
	bridge->reportStageStarted("Binary loading");
	bridge->reportProgress("Binary loading", 50);
	bridge->reportStageCompleted("Binary loading", 100);
	bridge->reportFunctionCount(42);
	bridge->reportThroughput(5.0, 20000.0);
	bridge->reportAnalysisCompleted(100LL);
	h.frame();
	EXPECT_EQ(h.ui.text(panel->functionCountLabel()), "42 fn");
	EXPECT_EQ(h.ui.text(panel->percentLabel()), "100%");
	EXPECT_EQ(h.ui.text(panel->elapsedLabel()), "100ms");
	EXPECT_EQ(panel->tracker().stages()[0].state, StageState::Done);
}

TEST_F(ProgressBridgeIntegrationTest, ResetAllAfterBridgeRun)
{
	bridge->reportStageStarted("Binary loading");
	bridge->reportStageCompleted("Binary loading", 100);
	h.frame();
	EXPECT_NO_THROW(panel->resetAll());
}

TEST_F(ProgressBridgeIntegrationTest, ReportsFromWorkerThreads)
{
	std::atomic<bool> go{false};
	std::vector<std::thread> workers;
	for (int t = 0; t < 4; ++t)
		workers.emplace_back([&, t] {
			while (!go)
				std::this_thread::yield();
			for (int i = 0; i < 50; ++i)
				bridge->reportStageCompleted("worker " + std::to_string(t), i + 1);
		});
	go = true;
	for (auto& th: workers)
		th.join();
	for (int i = 0; i < 4; ++i)
		h.frame();
	for (int t = 0; t < 4; ++t)
	{
		const int idx = panel->tracker().stageIndex("worker " + std::to_string(t));
		ASSERT_GE(idx, 0);
		EXPECT_EQ(panel->tracker().stages()[static_cast<std::size_t>(idx)].elapsedMs, 50);
	}
}

TEST_F(ProgressBridgeIntegrationTest, PanelDestroyedWithReportsPending)
{
	bridge->reportFunctionCount(5);
	panel.reset();
	EXPECT_EQ(bridge->listenerCount(), 0u);
	EXPECT_NO_THROW(h.frame()); // the posted drain finds the panel gone
	bridge->reportFunctionCount(6);
	h.frame();
}

// ─── Tracker ─────────────────────────────────────────────────────────────────

TEST(ProgressTrackerTest, FormattingHelpers)
{
	EXPECT_EQ(model::formatStageMs(-1), "");
	EXPECT_EQ(model::formatStageMs(0), "0ms");
	EXPECT_EQ(model::formatStageMs(999), "999ms");
	EXPECT_EQ(model::formatStageMs(1000), "1.0s");
	EXPECT_EQ(model::formatStageMs(59'999), "59.9s");
	EXPECT_EQ(model::formatStageMs(61'500), "1m1s");
	EXPECT_EQ(model::formatInstructionCount(999), "999 instr");
	EXPECT_EQ(model::formatInstructionCount(1000), "1K instr");
	EXPECT_EQ(model::formatInstructionCount(1'000'000), "1M instr");
	EXPECT_EQ(model::formatFunctionRate(3.25), "3.2");
	EXPECT_EQ(model::formatInstructionRate(999'999.0), "999999");
	EXPECT_EQ(model::waterfallColor(0), 0x89b4fau);
	EXPECT_EQ(model::waterfallColor(14), 0x89b4fau);
}

TEST(ProgressTrackerTest, BudgetLevels)
{
	EXPECT_EQ(model::budgetLevel(1000, 0), BudgetLevel::Normal);
	EXPECT_EQ(model::budgetLevel(7999, 10), BudgetLevel::Normal);
	EXPECT_EQ(model::budgetLevel(8000, 10), BudgetLevel::Warning);
	EXPECT_EQ(model::budgetLevel(10'000, 10), BudgetLevel::Exceeded);
	EXPECT_EQ(model::budgetToolTip(BudgetLevel::Warning, 10), "Approaching the 10s analysis budget.");
	EXPECT_NE(model::budgetToolTip(BudgetLevel::Exceeded, 10).find("press F6 to cancel"), std::string::npos);
}

TEST(ProgressTrackerTest, TimingsWaterfallAndJson)
{
	int64_t now = 1000;
	ProgressTracker t;
	t.setClock([&] { return now; });
	t.analysisStarted();
	t.stageStarted("Binary loading");
	now += 120;
	t.stageCompleted("Binary loading", 120);
	t.stageStarted("String detection");
	now += 30;
	t.stageErrored("String detection", "bad section");
	t.stageCompleted("SSA construction", 50); // never started: no bar
	EXPECT_EQ(t.timings()[0].startMs, 0);
	EXPECT_EQ(t.timings()[2].startMs, 120);
	const auto bars = t.waterfall();
	ASSERT_EQ(bars.size(), 1u);
	EXPECT_EQ(bars[0].label, "Binary loading");
	EXPECT_EQ(bars[0].durationMs, 120);
	EXPECT_EQ(bars[0].color, model::waterfallColor(0));
	EXPECT_EQ(t.waterfallEnd(), 120);
	EXPECT_EQ(t.stages()[2].message, "bad section");
	EXPECT_EQ(t.overallPercent(), 3 * 100 / 14);
	EXPECT_EQ(t.elapsedMs(), 150);
	t.analysisCancelled();
	EXPECT_FALSE(t.running());
	now += 1000;
	EXPECT_EQ(t.elapsedMs(), 150);

	t.setFunctionCount(3);
	t.setInstructionCount(70);
	const std::string json = t.exportJson();
	// Keys sorted, four-space indent (the Qt QJsonDocument::Indented layout).
	EXPECT_EQ(
		json.substr(0, json.find("\"stages\"")),
		"{\n    \"elapsed_ms\": 150,\n    \"function_count\": 3,\n    \"instruction_count\": 70,\n    ");
	EXPECT_NE(
		json.find(
			"        {\n            \"elapsed_ms\": 120,\n            \"name\": \"Binary loading\",\n            "
			"\"state\": \"done\"\n        },"),
		std::string::npos);
	EXPECT_NE(
		json.find(
			"\"timeline\": [\n        {\n            \"duration_ms\": 120,\n            \"name\": \"Binary "
			"loading\",\n            \"start_ms\": 0\n        },"),
		std::string::npos);
	EXPECT_EQ(json.back(), '\n');
	const auto v = forge::json::parse(json);
	EXPECT_EQ(v["timeline"].arr().size(), 2u);
	EXPECT_EQ(v["timeline"][1]["duration_ms"].i(), -1);
}

// ─── Headless UI ─────────────────────────────────────────────────────────────

TEST(ProgressPanelUiTest, ElapsedTickBudgetAndExport)
{
	Headless h({900.f, 640.f});
	ProgressPanel panel(h.ui, h.ui.root());
	int64_t now = 0;
	panel.setClock([&] { return now; });
	panel.setAnalysisBudgetSecs(10);
	panel.analysisStarted();
	now = 2500;
	h.ui.update(1.1f); // the 1 s timer fires
	EXPECT_EQ(h.ui.text(panel.elapsedLabel()), "2.5s");
	EXPECT_EQ(panel.budgetState(), BudgetLevel::Normal);
	now = 8500;
	h.ui.update(1.1f);
	EXPECT_EQ(panel.budgetState(), BudgetLevel::Warning);
	EXPECT_EQ(panel.elapsedToolTip(), "Approaching the 10s analysis budget.");
	now = 12'000;
	h.ui.update(1.1f);
	EXPECT_EQ(panel.budgetState(), BudgetLevel::Exceeded);
	panel.analysisCompleted(12'345);
	EXPECT_EQ(h.ui.text(panel.elapsedLabel()), "12.3s");
	EXPECT_EQ(panel.budgetState(), BudgetLevel::Normal);

	// A running stage without a percentage shows FuseUI's busy (indeterminate) bar.
	panel.stageStarted("SSA construction");
	EXPECT_TRUE(fuse::ui::widgets::progressIndeterminate(h.ui, panel.stageRow("SSA construction").bar));
	EXPECT_TRUE(h.ui.hasClass(panel.stageRow("SSA construction").bar, "indeterminate"));
	EXPECT_FALSE(fuse::ui::widgets::progressIndeterminate(h.ui, panel.stageRow("Binary loading").bar));

	int exports = 0;
	panel.onExportRequested = [&] { ++exports; };
	h.clickNode(panel.exportButton());
	EXPECT_EQ(exports, 1);
	std::string clip;
	ASSERT_TRUE(h.ui.clipboardText(clip));
	EXPECT_EQ(clip, panel.exportJson());
	const auto dir = std::filesystem::temp_directory_path() / "retdec-gui-progress-test";
	std::filesystem::remove_all(dir);
	std::filesystem::create_directories(dir);
	ASSERT_TRUE(w::fileDialogNavigate(h.ui, panel.fileDialogNode(), dir.string()));
	h.frame();
	h.clickNode(h.findByText("Save"));
	EXPECT_TRUE(std::filesystem::exists(dir / "progress.json"));
	std::filesystem::remove_all(dir);
}

TEST(ProgressPanelUiTest, ErrorTooltipCancelAndRender)
{
	Headless h({900.f, 640.f});
	ProgressPanel panel(h.ui, h.ui.root());
	int64_t now = 0;
	panel.setClock([&] { return now; });
	panel.analysisStarted();
	const char* names[] = {
		"Binary loading", "RTTI reconstruction", "String detection", "SSA construction", "Variable recovery"};
	const int64_t durations[] = {120, 340, 80, 900, 450};
	for (int i = 0; i < 5; ++i)
	{
		panel.stageStarted(names[i]);
		now += durations[i];
		panel.stageCompleted(names[i], durations[i]);
	}
	panel.stageStarted("Alias analysis");
	panel.stageProgress("Alias analysis", 60);
	panel.stageStarted("Type inference");
	panel.stageErrored("CFG structuring", "irreducible loop");
	panel.setFunctionCount(312);
	panel.setInstructionCount(48'200);
	panel.setThroughput(41.6, 1'250'000.0);
	h.frame();
	EXPECT_EQ(panel.waterfallBars().size(), 5u);
	EXPECT_EQ(h.ui.text(panel.stageRow("Alias analysis").time), "");
	EXPECT_GT(h.render("progress_panel.png"), 1000u);

	panel.analysisCancelled();
	EXPECT_EQ(panel.tracker().stages()[5].state, StageState::Pending);
	EXPECT_EQ(panel.tracker().stages()[7].state, StageState::Error);
	panel.resetAll();
	h.frame();
	EXPECT_TRUE(panel.waterfallBars().empty());
}
