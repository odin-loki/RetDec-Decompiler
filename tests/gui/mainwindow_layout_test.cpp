/**
 * @file tests/gui/mainwindow_layout_test.cpp
 * @brief Layout invariants for the v3-simplified RetDecMainWindow.
 *
 *  * Central area = TriageBanner + QTabWidget of documents.
 *  * Centre tab order: Decompiled C | Assembly | IR (SSA) | CFG | Synced.
 *  * Left dock = Functions (single panel, no inner tab strip).
 *  * Right dock = Workspace tabbed: Strings | Inspect (only).
 *  * Bottom dock = Output tabbed: Console | Problems.
 *  * No mode toolbar.
 */

#include "retdec/gui/mainwindow.h"
#include "retdec/gui/panels/live_console_panel.h"
#include "retdec/gui/panels/triage_banner.h"

#include <gtest/gtest.h>

#include <QApplication>
#include <QDockWidget>
#include <QTabWidget>
#include <QToolBar>
#include <QtGlobal>

#include <memory>

using retdec::gui::RetDecMainWindow;

class MainWindowLayoutTest : public ::testing::Test {
protected:
    void SetUp() override {
        Q_ASSERT(QApplication::instance() != nullptr);
        win = std::make_unique<RetDecMainWindow>();
    }
    void TearDown() override {
        if (QApplication::instance())
            QApplication::processEvents();
        win.reset();
    }
    std::unique_ptr<RetDecMainWindow> win;
};

TEST_F(MainWindowLayoutTest, ConstructsAndHasCentralTabs) {
    auto* docs = win->documentTabsForTest();
    ASSERT_NE(docs, nullptr);
    EXPECT_EQ(docs->count(), 5);
    EXPECT_EQ(docs->tabText(0), QStringLiteral("Decompiled C"));
    EXPECT_EQ(docs->tabText(1), QStringLiteral("Assembly"));
    EXPECT_EQ(docs->tabText(2), QStringLiteral("IR (SSA)"));
    EXPECT_EQ(docs->tabText(3), QStringLiteral("CFG"));
    EXPECT_TRUE(docs->tabText(4).contains(QStringLiteral("Synced")));
}

TEST_F(MainWindowLayoutTest, BottomDockHasConsoleAndProblemsTabs) {
    auto* output = win->outputDockForTest();
    ASSERT_NE(output, nullptr);
    auto* tabs = win->outputTabsForTest();
    ASSERT_NE(tabs, nullptr);
    EXPECT_EQ(output->widget(), tabs);
    EXPECT_EQ(tabs->count(), 2);
    EXPECT_EQ(tabs->tabText(0), QStringLiteral("Console"));
    EXPECT_EQ(tabs->tabText(1), QStringLiteral("Problems"));
    EXPECT_EQ(tabs->widget(0), win->liveConsoleForTest());
}

TEST_F(MainWindowLayoutTest, LeftFunctionsDockIsSinglePanelNotTabbed) {
    auto* functions = win->symbolsDockForTest();
    ASSERT_NE(functions, nullptr);
    EXPECT_EQ(functions->objectName(), QStringLiteral("dock_functions"));
    // No inner QTabWidget — the v3 design drops the Functions/Types tab strip.
    EXPECT_EQ(functions->findChildren<QTabWidget*>().size(), 0);
}

TEST_F(MainWindowLayoutTest, RightWorkspaceHasOnlyStringsAndInspect) {
    auto* ws = win->workspaceTabsForTest();
    ASSERT_NE(ws, nullptr);
    EXPECT_EQ(ws->count(), 2);
    EXPECT_EQ(ws->tabText(0), QStringLiteral("Strings"));
    EXPECT_EQ(ws->tabText(1), QStringLiteral("Inspect"));
}

TEST_F(MainWindowLayoutTest, DocksAreNamedForLayoutPersistence) {
    auto* output    = win->outputDockForTest();
    auto* functions = win->symbolsDockForTest();
    auto* workspace = win->workspaceDockForTest();
    ASSERT_NE(output,    nullptr);
    ASSERT_NE(functions, nullptr);
    ASSERT_NE(workspace, nullptr);
    EXPECT_FALSE(output->objectName().isEmpty());
    EXPECT_FALSE(functions->objectName().isEmpty());
    EXPECT_FALSE(workspace->objectName().isEmpty());
}

TEST_F(MainWindowLayoutTest, NoModeToolbarRemains) {
    // The v3 redesign drops the four-button mode strip. No toolbar should
    // have the "Mode" object name (or be titled "Mode").
    for (QToolBar* tb : win->findChildren<QToolBar*>()) {
        EXPECT_NE(tb->objectName(), QStringLiteral("modeToolbar"));
        EXPECT_NE(tb->windowTitle(), QStringLiteral("Mode"));
    }
}

TEST_F(MainWindowLayoutTest, TriageBannerStartsHidden) {
    auto* tb = win->triageBannerForTest();
    ASSERT_NE(tb, nullptr);
    EXPECT_TRUE(tb->isHidden());
}

TEST_F(MainWindowLayoutTest, AiAssistantDockIsNotPresent) {
    // The AI assistant was removed from the v3.1 layout — it floated
    // and disrupted the working area. No dock with that name should exist.
    const QList<QDockWidget*> docks = win->findChildren<QDockWidget*>();
    for (const QDockWidget* d : docks) {
        EXPECT_NE(d->windowTitle().compare(QStringLiteral("AI Assistant")), 0);
        EXPECT_NE(d->objectName().compare(QStringLiteral("dock_ai_assistant")), 0);
    }
}
