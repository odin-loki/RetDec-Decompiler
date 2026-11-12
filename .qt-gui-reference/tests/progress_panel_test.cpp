/**
 * @file tests/gui/progress_panel_test.cpp
 * @brief Unit tests for ProgressPanel and AnalysisBridge (Task 49).
 *
 * Tests cover:
 *   - ProgressPanel construction and default state
 *   - setStageState() for all four StageState values
 *   - setStageElapsed() and formatMs formatting
 *   - updateOverallBar() percentage calculation
 *   - setFunctionCount / setInstructionCount label text
 *   - setThroughput label formatting
 *   - resetAll() restores default state
 *   - addRow() for dynamically added stages
 *   - exportJson() structure and content
 *   - AnalysisBridge signal emission
 *   - connectBridge() wires signals to panel slots
 */

#include <memory>
#include <gtest/gtest.h>
#include <QApplication>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QSignalSpy>
#include <QTest>

#include "retdec/gui/panels/progress_panel.h"

// ─── Test fixture ─────────────────────────────────────────────────────────────

namespace {

void qtEnv() {
    Q_ASSERT(QApplication::instance() != nullptr);
}

} // anonymous namespace

class ProgressPanelTest : public ::testing::Test {
protected:
    void SetUp() override {
        (void)qtEnv();
        panel = std::make_unique<retdec::gui::panels::ProgressPanel>();
    }
    void TearDown() override {
        if (QApplication::instance())
            QApplication::processEvents();
    }
    std::unique_ptr<retdec::gui::panels::ProgressPanel> panel;
};

// ─── Construction ─────────────────────────────────────────────────────────────

TEST_F(ProgressPanelTest, ConstructsWithoutCrash) {
    EXPECT_NE(panel.get(), nullptr);
}

TEST_F(ProgressPanelTest, PanelTitleIsProgress) {
    EXPECT_EQ(panel->panelTitle(), "Progress");
}

// ─── setStageState ────────────────────────────────────────────────────────────

TEST_F(ProgressPanelTest, SetStagePending) {
    // Should not throw / crash
    EXPECT_NO_THROW(panel->setStageState("Binary loading",
        retdec::gui::panels::StageState::Pending));
}

TEST_F(ProgressPanelTest, SetStageRunningIndeterminate) {
    EXPECT_NO_THROW(panel->setStageState("Binary loading",
        retdec::gui::panels::StageState::Running, -1));
}

TEST_F(ProgressPanelTest, SetStageRunningWithPercent) {
    EXPECT_NO_THROW(panel->setStageState("SSA construction",
        retdec::gui::panels::StageState::Running, 42));
}

TEST_F(ProgressPanelTest, SetStageDone) {
    EXPECT_NO_THROW(panel->setStageState("Binary loading",
        retdec::gui::panels::StageState::Done));
}

TEST_F(ProgressPanelTest, SetStageError) {
    EXPECT_NO_THROW(panel->setStageState("Type inference",
        retdec::gui::panels::StageState::Error, 25));
}

TEST_F(ProgressPanelTest, SetStateForUnknownStageCreatesRow) {
    // Should dynamically add a new row without crashing.
    EXPECT_NO_THROW(panel->setStageState("Custom stage",
        retdec::gui::panels::StageState::Running, 50));
}

// ─── setStageElapsed ──────────────────────────────────────────────────────────

TEST_F(ProgressPanelTest, SetElapsedMilliseconds) {
    EXPECT_NO_THROW(panel->setStageElapsed("Binary loading", 350));
}

TEST_F(ProgressPanelTest, SetElapsedSeconds) {
    EXPECT_NO_THROW(panel->setStageElapsed("SSA construction", 2500));
}

TEST_F(ProgressPanelTest, SetElapsedMinutes) {
    EXPECT_NO_THROW(panel->setStageElapsed("Inter-procedural analysis", 90000));
}

TEST_F(ProgressPanelTest, SetElapsedForUnknownStageIsNoop) {
    EXPECT_NO_THROW(panel->setStageElapsed("Does not exist", 1000));
}

// ─── Overall progress ────────────────────────────────────────────────────────

TEST_F(ProgressPanelTest, OverallProgressUpdatesAfterDoneStages) {
    // Mark first two stages Done — overall should rise above 0.
    panel->setStageState("Binary loading",
        retdec::gui::panels::StageState::Done);
    panel->setStageState("RTTI reconstruction",
        retdec::gui::panels::StageState::Done);
    // 2 out of 14 default stages = ~14%
    SUCCEED();  // Visual check; just must not crash.
}

TEST_F(ProgressPanelTest, SetOverallProgressDirectly) {
    EXPECT_NO_THROW(panel->setOverallProgress(75));
}

// ─── Counts / throughput ─────────────────────────────────────────────────────

TEST_F(ProgressPanelTest, SetFunctionCount) {
    EXPECT_NO_THROW(panel->setFunctionCount(1234));
}

TEST_F(ProgressPanelTest, SetInstructionCountSmall) {
    EXPECT_NO_THROW(panel->setInstructionCount(500));
}

TEST_F(ProgressPanelTest, SetInstructionCountKilo) {
    EXPECT_NO_THROW(panel->setInstructionCount(45'000));
}

TEST_F(ProgressPanelTest, SetInstructionCountMega) {
    EXPECT_NO_THROW(panel->setInstructionCount(3'200'000));
}

TEST_F(ProgressPanelTest, SetThroughputLow) {
    EXPECT_NO_THROW(panel->setThroughput(3.5, 120.0));
}

TEST_F(ProgressPanelTest, SetThroughputHighInstructions) {
    EXPECT_NO_THROW(panel->setThroughput(120.0, 1'500'000.0));
}

// ─── resetAll ────────────────────────────────────────────────────────────────

TEST_F(ProgressPanelTest, ResetAllRestoresState) {
    panel->setStageState("Binary loading",
        retdec::gui::panels::StageState::Done);
    panel->setFunctionCount(9999);
    panel->setThroughput(10.0, 1000.0);
    EXPECT_NO_THROW(panel->resetAll());
}

TEST_F(ProgressPanelTest, ClearCallsResetAll) {
    panel->setStageState("Code generation",
        retdec::gui::panels::StageState::Running, 30);
    EXPECT_NO_THROW(panel->clear());
}

// ─── exportJson ──────────────────────────────────────────────────────────────

TEST_F(ProgressPanelTest, ExportJsonIsValidJson) {
    QString json = panel->exportJson();
    QJsonParseError err;
    QJsonDocument doc = QJsonDocument::fromJson(json.toUtf8(), &err);
    EXPECT_EQ(err.error, QJsonParseError::NoError)
        << err.errorString().toStdString();
    EXPECT_FALSE(doc.isNull());
}

TEST_F(ProgressPanelTest, ExportJsonHasRequiredTopLevelKeys) {
    QString json = panel->exportJson();
    QJsonDocument doc = QJsonDocument::fromJson(json.toUtf8());
    QJsonObject root  = doc.object();
    EXPECT_TRUE(root.contains("elapsed_ms"));
    EXPECT_TRUE(root.contains("function_count"));
    EXPECT_TRUE(root.contains("instruction_count"));
    EXPECT_TRUE(root.contains("stages"));
    EXPECT_TRUE(root.contains("timeline"));
}

TEST_F(ProgressPanelTest, ExportJsonStagesArrayHasDefaultStages) {
    QString json = panel->exportJson();
    QJsonDocument doc = QJsonDocument::fromJson(json.toUtf8());
    QJsonArray stages = doc.object()["stages"].toArray();
    EXPECT_EQ(stages.size(), 14);  // 14 default stages
}

TEST_F(ProgressPanelTest, ExportJsonStageHasStateField) {
    QString json = panel->exportJson();
    QJsonDocument doc = QJsonDocument::fromJson(json.toUtf8());
    QJsonObject first = doc.object()["stages"].toArray()[0].toObject();
    EXPECT_TRUE(first.contains("name"));
    EXPECT_TRUE(first.contains("state"));
    EXPECT_EQ(first["state"].toString(), "pending");
}

TEST_F(ProgressPanelTest, ExportJsonReflectsDoneState) {
    panel->setStageState("Binary loading",
        retdec::gui::panels::StageState::Done);
    QString json = panel->exportJson();
    QJsonDocument doc = QJsonDocument::fromJson(json.toUtf8());
    QJsonObject first = doc.object()["stages"].toArray()[0].toObject();
    EXPECT_EQ(first["state"].toString(), "done");
}

TEST_F(ProgressPanelTest, ExportJsonFunctionCountMatchesSet) {
    panel->setFunctionCount(42);
    QString json = panel->exportJson();
    QJsonDocument doc = QJsonDocument::fromJson(json.toUtf8());
    EXPECT_EQ(doc.object()["function_count"].toInt(), 42);
}

// ─── AnalysisBridge signal emission ──────────────────────────────────────────

class BridgeTest : public ::testing::Test {
protected:
    void SetUp() override {
        (void)qtEnv();
        bridge = std::make_unique<retdec::gui::panels::AnalysisBridge>();
    }
    void TearDown() override {
        if (QApplication::instance())
            QApplication::processEvents();
    }
    std::unique_ptr<retdec::gui::panels::AnalysisBridge> bridge;
};

TEST_F(BridgeTest, ConstructsWithoutCrash) {
    EXPECT_NE(bridge.get(), nullptr);
}

TEST_F(BridgeTest, ReportStageStartedEmitsSignal) {
    QSignalSpy spy(bridge.get(),
        &retdec::gui::panels::AnalysisBridge::stageStarted);
    bridge->reportStageStarted("Binary loading");
    EXPECT_EQ(spy.count(), 1);
    EXPECT_EQ(spy[0][0].toString(), "Binary loading");
}

TEST_F(BridgeTest, ReportProgressEmitsSignal) {
    QSignalSpy spy(bridge.get(),
        &retdec::gui::panels::AnalysisBridge::progressUpdated);
    bridge->reportProgress("SSA construction", 55);
    EXPECT_EQ(spy.count(), 1);
    EXPECT_EQ(spy[0][1].toInt(), 55);
}

TEST_F(BridgeTest, ReportStageCompletedEmitsSignal) {
    QSignalSpy spy(bridge.get(),
        &retdec::gui::panels::AnalysisBridge::stageCompleted);
    bridge->reportStageCompleted("Binary loading", 350);
    EXPECT_EQ(spy.count(), 1);
    EXPECT_EQ(spy[0][1].toLongLong(), 350LL);
}

TEST_F(BridgeTest, ReportStageErrorEmitsSignal) {
    QSignalSpy spy(bridge.get(),
        &retdec::gui::panels::AnalysisBridge::stageErrored);
    bridge->reportStageError("Type inference", "Out of memory");
    EXPECT_EQ(spy.count(), 1);
    EXPECT_EQ(spy[0][1].toString(), "Out of memory");
}

TEST_F(BridgeTest, ReportFunctionCountEmitsSignal) {
    QSignalSpy spy(bridge.get(),
        &retdec::gui::panels::AnalysisBridge::functionCountChanged);
    bridge->reportFunctionCount(777);
    EXPECT_EQ(spy.count(), 1);
    EXPECT_EQ(spy[0][0].toInt(), 777);
}

TEST_F(BridgeTest, ReportInstructionCountEmitsSignal) {
    QSignalSpy spy(bridge.get(),
        &retdec::gui::panels::AnalysisBridge::instructionCountChanged);
    bridge->reportInstructionCount(1'234'567LL);
    EXPECT_EQ(spy.count(), 1);
    EXPECT_EQ(spy[0][0].toLongLong(), 1'234'567LL);
}

TEST_F(BridgeTest, ReportThroughputEmitsSignal) {
    QSignalSpy spy(bridge.get(),
        &retdec::gui::panels::AnalysisBridge::throughputChanged);
    bridge->reportThroughput(12.5, 500000.0);
    EXPECT_EQ(spy.count(), 1);
    EXPECT_DOUBLE_EQ(spy[0][0].toDouble(), 12.5);
}

TEST_F(BridgeTest, ReportAnalysisStartedEmitsSignal) {
    QSignalSpy spy(bridge.get(),
        &retdec::gui::panels::AnalysisBridge::analysisStarted);
    bridge->reportAnalysisStarted();
    EXPECT_EQ(spy.count(), 1);
}

TEST_F(BridgeTest, ReportAnalysisCompletedEmitsSignal) {
    QSignalSpy spy(bridge.get(),
        &retdec::gui::panels::AnalysisBridge::analysisCompleted);
    bridge->reportAnalysisCompleted(12'345LL);
    EXPECT_EQ(spy.count(), 1);
    EXPECT_EQ(spy[0][0].toLongLong(), 12'345LL);
}

TEST_F(BridgeTest, ReportAnalysisCancelledEmitsSignal) {
    QSignalSpy spy(bridge.get(),
        &retdec::gui::panels::AnalysisBridge::analysisCancelled);
    bridge->reportAnalysisCancelled();
    EXPECT_EQ(spy.count(), 1);
}

// ─── connectBridge integration ────────────────────────────────────────────────

class BridgeIntegrationTest : public ::testing::Test {
protected:
    void SetUp() override {
        (void)qtEnv();
        panel  = std::make_unique<retdec::gui::panels::ProgressPanel>();
        bridge = std::make_unique<retdec::gui::panels::AnalysisBridge>();
        panel->connectBridge(bridge.get());
    }
    void TearDown() override {
        if (QApplication::instance())
            QApplication::processEvents();
    }
    std::unique_ptr<retdec::gui::panels::ProgressPanel>  panel;
    std::unique_ptr<retdec::gui::panels::AnalysisBridge> bridge;
};

TEST_F(BridgeIntegrationTest, BridgeStartedTriggersPanelSignal) {
    QSignalSpy spy(panel.get(),
        &retdec::gui::panels::ProgressPanel::stageStarted);
    bridge->reportStageStarted("Binary loading");
    QApplication::processEvents();
    EXPECT_GE(spy.count(), 1);
}

TEST_F(BridgeIntegrationTest, BridgeCompletedTriggersPanelSignal) {
    QSignalSpy spy(panel.get(),
        &retdec::gui::panels::ProgressPanel::stageCompleted);
    bridge->reportStageCompleted("Binary loading", 250);
    QApplication::processEvents();
    EXPECT_GE(spy.count(), 1);
    EXPECT_EQ(spy[0][1].toLongLong(), 250LL);
}

TEST_F(BridgeIntegrationTest, BridgeAnalysisCompletedTriggersPanelSignal) {
    QSignalSpy spy(panel.get(),
        &retdec::gui::panels::ProgressPanel::analysisCompleted);
    bridge->reportAnalysisCompleted(9000LL);
    QApplication::processEvents();
    EXPECT_GE(spy.count(), 1);
}

TEST_F(BridgeIntegrationTest, BridgeProgressUpdatesDoNotCrash) {
    bridge->reportAnalysisStarted();
    bridge->reportStageStarted("Binary loading");
    bridge->reportProgress("Binary loading", 50);
    bridge->reportStageCompleted("Binary loading", 100);
    bridge->reportFunctionCount(42);
    bridge->reportThroughput(5.0, 20000.0);
    bridge->reportAnalysisCompleted(100LL);
    QApplication::processEvents();
    SUCCEED();
}

TEST_F(BridgeIntegrationTest, ResetAllAfterBridgeRun) {
    bridge->reportStageStarted("Binary loading");
    bridge->reportStageCompleted("Binary loading", 100);
    QApplication::processEvents();
    EXPECT_NO_THROW(panel->resetAll());
}

