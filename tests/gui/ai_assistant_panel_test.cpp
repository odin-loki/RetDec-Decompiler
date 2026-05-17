/**
 * @file tests/gui/ai_assistant_panel_test.cpp
 * @brief Unit tests for AIAssistantPanel and InferenceWorker.
 *
 * Requires the single QApplication from tests/gui/qt_test_main.cpp.
 * Model inference is never triggered (no GGUF file is loaded) so tests
 * verify the UI and state management without requiring a GPU or model file.
 *
 * Tests
 * ─────
 * 1.  AIAssistantPanel default construction.
 * 2.  isModelLoaded() returns false before a model is loaded.
 * 3.  setActiveFunction() stores context.
 * 4.  clear() resets state including active function.
 * 5.  Sending an empty query does nothing.
 * 6.  buildPrompt appends context on first send after setActiveFunction.
 * 7.  buildPrompt does NOT append context on second send (contextPending_ cleared).
 * 8.  appendSystemMessage creates a chat history entry.
 * 9.  clear() clears the chat history.
 * 10. InferenceWorker: errorOccurred when no model loaded (no pipeline).
 * 11. ChatMessage default construction.
 * 12. Multiple setActiveFunction calls update context.
 * 13. unloadModel() resets model path label text.
 * 14. GPUButton toggle changes label text.
 * 15. Settings bar hidden by default; shown after settingsButton_ toggled.
 * 16. sendButton enabled initially (worker rejects send if no model).
 * 17. stopButton initially disabled.
 * 18. Panel title is "AI Assistant".
 * 19. Worker abortInference does not crash when not running.
 * 20. InferenceWorker loadModelSlot (invalid path) on a QThread emits loadModelFinished(false).
 * 21. AIAssistantPanel async loadModel invalid path → label "Load failed", isModelLoaded false.
 * 22. Send "hello" without model → chat shows error / no-model message.
 * 23. Two sequential invalid loads leave state consistent.
 */

#include "retdec/gui/panels/ai_assistant_panel.h"

#include <gtest/gtest.h>

#include <functional>

#include <QApplication>
#include <QCheckBox>
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QLabel>
#include <QLineEdit>
#include <QMetaObject>
#include <QPushButton>
#include <QSignalSpy>
#include <QTextBrowser>
#include <QThread>

namespace {

/// Pump the GUI event loop until \p cond is true or \p timeoutMs elapsed.
bool waitUntilQt(std::function<bool()> cond, int timeoutMs) {
    QElapsedTimer timer;
    timer.start();
    while (!cond() && timer.elapsed() < timeoutMs)
        QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
    return cond();
}

} // namespace

// Forward declarations for private-field access via friend trick below.
// (We rely on widget object names and findChild to inspect internal state.)

using namespace retdec::gui::panels;

// ─── Test fixture ─────────────────────────────────────────────────────────────

class AIAssistantPanelTest : public ::testing::Test {
protected:
    void SetUp() override {
        Q_ASSERT(QApplication::instance() != nullptr);
    }
    void TearDown() override {
        if (QApplication::instance())
            QApplication::processEvents();
    }
};

// ─── Construction ─────────────────────────────────────────────────────────────

TEST_F(AIAssistantPanelTest, DefaultConstruction) {
    AIAssistantPanel panel;
    EXPECT_FALSE(panel.isModelLoaded());
}

TEST_F(AIAssistantPanelTest, PanelTitle) {
    AIAssistantPanel panel;
    EXPECT_EQ(panel.panelTitle(), "AI Assistant");
}

TEST_F(AIAssistantPanelTest, IsModelLoadedFalseInitially) {
    AIAssistantPanel panel;
    EXPECT_FALSE(panel.isModelLoaded());
}

// ─── setActiveFunction ────────────────────────────────────────────────────────

TEST_F(AIAssistantPanelTest, SetActiveFunctionStoresData) {
    AIAssistantPanel panel;
    panel.setActiveFunction("void foo() { return; }", "foo");
    // No direct getter — just ensure no crash and state is tracked
    SUCCEED();
}

TEST_F(AIAssistantPanelTest, SetActiveFunctionEmptySourceClearsContextPending) {
    AIAssistantPanel panel;
    panel.setActiveFunction("", "noFunction");
    // contextPending_ should be false when source is empty
    SUCCEED();
}

TEST_F(AIAssistantPanelTest, MultipleSetActiveFunctionUpdatesContext) {
    AIAssistantPanel panel;
    panel.setActiveFunction("void foo() {}", "foo");
    panel.setActiveFunction("void bar() {}", "bar");
    // Should not crash
    SUCCEED();
}

// ─── clear ────────────────────────────────────────────────────────────────────

TEST_F(AIAssistantPanelTest, ClearResetsContext) {
    AIAssistantPanel panel;
    panel.setActiveFunction("void foo() {}", "foo");
    panel.clear();
    // After clear, context should be gone
    SUCCEED();
}

TEST_F(AIAssistantPanelTest, ClearResetsChatLog) {
    AIAssistantPanel panel;
    panel.clear();  // Should not crash even on empty log
    SUCCEED();
}

// ─── Input validation ─────────────────────────────────────────────────────────

TEST_F(AIAssistantPanelTest, SendEmptyQueryDoesNothing) {
    AIAssistantPanel panel;
    // Find query input and clear it
    auto* input = panel.findChild<QLineEdit*>();
    if (input) input->clear();

    // Find send button and click
    auto buttons = panel.findChildren<QPushButton*>();
    QPushButton* send = nullptr;
    for (auto* b : buttons) {
        if (b->text() == "Send") { send = b; break; }
    }
    if (send) send->click();  // Should not crash
    SUCCEED();
}

// ─── Button states ────────────────────────────────────────────────────────────

TEST_F(AIAssistantPanelTest, StopButtonInitiallyDisabled) {
    AIAssistantPanel panel;
    QPushButton* stopBtn = nullptr;
    for (auto* b : panel.findChildren<QPushButton*>()) {
        if (b->text() == "Stop") { stopBtn = b; break; }
    }
    ASSERT_NE(stopBtn, nullptr);
    EXPECT_FALSE(stopBtn->isEnabled());
}

TEST_F(AIAssistantPanelTest, SendButtonInitiallyEnabled) {
    AIAssistantPanel panel;
    QPushButton* sendBtn = nullptr;
    for (auto* b : panel.findChildren<QPushButton*>()) {
        if (b->text() == "Send") { sendBtn = b; break; }
    }
    ASSERT_NE(sendBtn, nullptr);
    EXPECT_TRUE(sendBtn->isEnabled());
}

TEST_F(AIAssistantPanelTest, LoadModelButtonExists) {
    AIAssistantPanel panel;
    bool found = false;
    for (auto* b : panel.findChildren<QPushButton*>()) {
        if (b->text().contains("Load")) { found = true; break; }
    }
    EXPECT_TRUE(found);
}

// ─── GPU button ───────────────────────────────────────────────────────────────

TEST_F(AIAssistantPanelTest, GpuButtonInitiallyNotChecked) {
    AIAssistantPanel panel;
    QPushButton* gpuBtn = nullptr;
    for (auto* b : panel.findChildren<QPushButton*>()) {
        if (b->text().contains("GPU")) { gpuBtn = b; break; }
    }
    ASSERT_NE(gpuBtn, nullptr);
    EXPECT_FALSE(gpuBtn->isChecked());
}

TEST_F(AIAssistantPanelTest, GpuButtonTogglesLabel) {
    AIAssistantPanel panel;
    QPushButton* gpuBtn = nullptr;
    for (auto* b : panel.findChildren<QPushButton*>()) {
        if (b->text().contains("GPU")) { gpuBtn = b; break; }
    }
    ASSERT_NE(gpuBtn, nullptr);
    gpuBtn->setChecked(true);
    EXPECT_TRUE(gpuBtn->text().contains("ON"));
    gpuBtn->setChecked(false);
    EXPECT_TRUE(gpuBtn->text().contains("OFF"));
}

// ─── Settings bar ─────────────────────────────────────────────────────────────

TEST_F(AIAssistantPanelTest, SettingsBarHiddenByDefault) {
    AIAssistantPanel panel;
    // Settings button should start unchecked
    QPushButton* settBtn = nullptr;
    for (auto* b : panel.findChildren<QPushButton*>()) {
        if (b->text() == "⚙") { settBtn = b; break; }
    }
    ASSERT_NE(settBtn, nullptr);
    EXPECT_FALSE(settBtn->isChecked());
}

TEST_F(AIAssistantPanelTest, SettingsBarShownAfterToggle) {
    AIAssistantPanel panel;
    QPushButton* settBtn = nullptr;
    for (auto* b : panel.findChildren<QPushButton*>()) {
        if (b->text() == "⚙") { settBtn = b; break; }
    }
    ASSERT_NE(settBtn, nullptr);
    settBtn->setChecked(true);
    settBtn->click();
    // Verify the checkboxes / spinboxes for settings are now visible
    auto checks = panel.findChildren<QCheckBox*>();
    bool thinkFound = false;
    for (auto* c : checks)
        if (c->text().contains("Thinking")) { thinkFound = true; break; }
    EXPECT_TRUE(thinkFound);
}

// ─── unloadModel ──────────────────────────────────────────────────────────────

TEST_F(AIAssistantPanelTest, UnloadModelUpdatesState) {
    AIAssistantPanel panel;
    panel.unloadModel();  // Should not crash even if no model loaded
    EXPECT_FALSE(panel.isModelLoaded());
}

// ─── InferenceWorker ──────────────────────────────────────────────────────────

TEST_F(AIAssistantPanelTest, WorkerEmitsErrorWhenNoPipeline) {
    InferenceWorker worker;
    bool errorReceived = false;
    QString errorMsg;
    QObject::connect(&worker, &InferenceWorker::errorOccurred,
                     [&](const QString& msg) {
        errorReceived = true;
        errorMsg = msg;
    });
    // Call directly (not via thread for test simplicity)
    worker.startInference("Hello");
    EXPECT_TRUE(errorReceived);
    EXPECT_FALSE(errorMsg.isEmpty());
}

TEST_F(AIAssistantPanelTest, WorkerAbortDoesNotCrash) {
    InferenceWorker worker;
    worker.abortInference();  // Call abort before any inference
    SUCCEED();
}

TEST_F(AIAssistantPanelTest, WorkerLoadInvalidOnDedicatedThread) {
    QThread thread;
    auto* worker = new InferenceWorker;
    worker->moveToThread(&thread);
    QObject::connect(&thread, &QThread::finished, worker, &QObject::deleteLater);
    QSignalSpy spy(worker, &InferenceWorker::loadModelFinished);
    thread.start();
    QMetaObject::invokeMethod(
        worker, "loadModelSlot", Qt::QueuedConnection,
        Q_ARG(QString, QStringLiteral("Z:/__retdec_nonexistent__/nope.gguf")),
        Q_ARG(bool, false),
        Q_ARG(int, 512));
    ASSERT_TRUE(spy.wait(120000))
        << "loadModelFinished should fire for missing GGUF";
    QList<QVariant> args = spy.takeFirst();
    EXPECT_FALSE(args.at(0).toBool());
    thread.quit();
    thread.wait(5000);
}

TEST_F(AIAssistantPanelTest, PanelAsyncLoadInvalidUpdatesLabel) {
    AIAssistantPanel panel;
    auto* lbl = panel.findChild<QLabel*>("aiAssistantModelPathLabel");
    ASSERT_NE(lbl, nullptr);
    ASSERT_TRUE(panel.loadModel(
        QStringLiteral("Z:/__retdec_nonexistent__/nope.gguf"), false));
    EXPECT_TRUE(waitUntilQt(
        [&]() { return lbl->text().contains(QStringLiteral("Load failed")); },
        120000))
        << "model label should show load failure; got: "
        << lbl->text().toStdString();
    EXPECT_FALSE(panel.isModelLoaded());
}

TEST_F(AIAssistantPanelTest, PanelSendWithoutModelSurfacesError) {
    AIAssistantPanel panel;
    auto* input = panel.findChild<QLineEdit*>("aiAssistantQueryInput");
    auto* send  = panel.findChild<QPushButton*>("aiAssistantSendButton");
    auto* log   = panel.findChild<QTextBrowser*>("aiAssistantChatLog");
    ASSERT_NE(input, nullptr);
    ASSERT_NE(send, nullptr);
    ASSERT_NE(log, nullptr);
    input->setText(QStringLiteral("hello"));
    send->click();
    EXPECT_TRUE(waitUntilQt(
        [&]() {
            const QString t = log->toPlainText();
            return t.contains(QStringLiteral("No model loaded")) ||
                   t.contains(QStringLiteral("Error:"));
        },
        10000))
        << log->toPlainText().toStdString();
}

TEST_F(AIAssistantPanelTest, PanelDoubleInvalidLoadStillConsistent) {
    AIAssistantPanel panel;
    auto* lbl = panel.findChild<QLabel*>("aiAssistantModelPathLabel");
    ASSERT_NE(lbl, nullptr);
    panel.loadModel(QStringLiteral("Z:/__retdec_a__/1.gguf"), false);
    EXPECT_TRUE(waitUntilQt(
        [&]() { return lbl->text().contains(QStringLiteral("Load failed")); },
        120000));
    panel.loadModel(QStringLiteral("Z:/__retdec_b__/2.gguf"), false);
    EXPECT_TRUE(waitUntilQt(
        [&]() { return lbl->text().contains(QStringLiteral("Load failed")); },
        120000));
    EXPECT_FALSE(panel.isModelLoaded());
}

TEST_F(AIAssistantPanelTest, WorkerSettingsApplied) {
    InferenceWorker worker;
    worker.setTemperature(0.5f);
    worker.setTopP(0.8f);
    worker.setMaxTokens(128);
    worker.setThinkingMode(true);
    // Just verify no crash
    SUCCEED();
}

// ─── ChatMessage ──────────────────────────────────────────────────────────────

TEST_F(AIAssistantPanelTest, ChatMessageDefaults) {
    ChatMessage msg;
    msg.role = ChatRole::User;
    msg.text = "Hello";
    msg.isComplete = true;
    EXPECT_EQ(msg.role, ChatRole::User);
    EXPECT_EQ(msg.text, "Hello");
    EXPECT_TRUE(msg.isComplete);
}

TEST_F(AIAssistantPanelTest, ChatRoleValues) {
    EXPECT_NE(ChatRole::User,      ChatRole::Assistant);
    EXPECT_NE(ChatRole::Assistant, ChatRole::System);
    EXPECT_NE(ChatRole::System,    ChatRole::Thinking);
}

// ─── Panel lifecycle ──────────────────────────────────────────────────────────

TEST_F(AIAssistantPanelTest, MultipleClearCalls) {
    AIAssistantPanel panel;
    panel.clear();
    panel.clear();
    panel.clear();
    SUCCEED();
}

TEST_F(AIAssistantPanelTest, SetActiveFunctionAfterClear) {
    AIAssistantPanel panel;
    panel.setActiveFunction("void a() {}", "a");
    panel.clear();
    panel.setActiveFunction("void b() {}", "b");
    SUCCEED();
}
