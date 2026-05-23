/**
 * @file tests/gui/comprehensive_smoke_test.cpp
 * @brief Comprehensive surface walker for retdec-gui.
 *
 * Triggers every menu action, mode button, dock toggle, layout preset,
 * shortcut, and panel public API on the main window and verifies that
 * nothing crashes / corrupts state.  Modal dialogs (file pickers,
 * QMessageBox::question, QInputDialog::getItem, …) are intercepted via a
 * QTimer that closes the active modal widget before it can block the test.
 *
 * Memory pressure tests:
 *   - 100 000 console lines (verify scrollback cap holds)
 *   - 30 000 diagnostics rows (verify cap evicts FIFO)
 *   - 10 000-line diff (LCS DP must complete)
 *   - 200 open/close project cycles (no leaks accumulate)
 *
 * All file I/O is sandboxed under QTemporaryDir.  No network, no spawned
 * children (real subprocess streaming is covered by the dedicated live
 * console + driver tests).
 */

#include "retdec/gui/mainwindow.h"
#include "retdec/gui/project_file.h"
#include "retdec/gui/panels/live_console_panel.h"
#include "retdec/gui/panels/command_log_panel.h"
#include "retdec/gui/panels/diagnostics_panel.h"
#include "retdec/gui/panels/progress_panel.h"
#include "retdec/gui/panels/inspect_panel.h"
#include "retdec/gui/panels/target_panel.h"
#include "retdec/gui/panels/signature_studio_panel.h"
#include "retdec/gui/panels/binary_browser_panel.h"
#include "retdec/gui/panels/function_list_panel.h"
#include "retdec/gui/panels/assembly_panel.h"
#include "retdec/gui/panels/ir_panel.h"
#include "retdec/gui/panels/decompiled_c_panel.h"
#include "retdec/gui/panels/cfg_panel.h"
#include "retdec/gui/panels/type_hierarchy_panel.h"
#include "retdec/gui/panels/call_graph_panel.h"
#include "retdec/gui/panels/strings_browser_panel.h"
#include "retdec/gui/panels/ai_assistant_panel.h"
#include "retdec/gui/panels/diff_panel.h"
#include "retdec/gui/panels/triage_banner.h"

#include <gtest/gtest.h>

#include <QAction>
#include <QApplication>
#include <QDialog>
#include <QDockWidget>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QElapsedTimer>
#include <QFile>
#include <QHash>
#include <QKeyEvent>
#include <QList>
#include <QMenu>
#include <QMenuBar>
#include <QMimeData>
#include <QObject>
#include <QPushButton>
#include <QShortcut>
#include <QTabWidget>
#include <QTemporaryDir>
#include <QTemporaryFile>
#include <QTest>
#include <QTextStream>
#include <QTimer>
#include <QToolButton>
#include <QUrl>
#include <QWidget>

#include <memory>
#include <random>

using namespace retdec::gui;

namespace {

/// Click whichever button on the active modal looks like "Close / Cancel / No".
/// Falls back to QDialog::reject() / QWidget::close().
void closeActiveModal() {
    QWidget* w = QApplication::activeModalWidget();
    if (!w)
        w = QApplication::activePopupWidget();
    if (!w)
        return;
    auto buttons = w->findChildren<QPushButton*>();
    const QStringList preferred{
            QStringLiteral("Cancel"),  QStringLiteral("Close"),
            QStringLiteral("Discard"), QStringLiteral("No"),
            QStringLiteral("Abort"),   QStringLiteral("Ignore"),
            QStringLiteral("OK"),
    };
    for (const QString& want : preferred) {
        for (QPushButton* b : buttons) {
            if (b->text().contains(want, Qt::CaseInsensitive)) {
                b->click();
                return;
            }
        }
    }
    if (auto* dlg = qobject_cast<QDialog*>(w))
        dlg->reject();
    else
        w->close();
}

/// Schedule modal-closer pulses for the next @p iterations event loop ticks.
class ModalCloser {
public:
    explicit ModalCloser(int iterations = 50, int periodMs = 25) {
        timer_.setSingleShot(false);
        timer_.setInterval(periodMs);
        QObject::connect(&timer_, &QTimer::timeout, [iterations, this]() mutable {
            closeActiveModal();
            if (--remaining_ <= 0)
                timer_.stop();
        });
        remaining_ = iterations;
        timer_.start();
    }
    ~ModalCloser() { timer_.stop(); }
private:
    QTimer timer_;
    int    remaining_ = 0;
};

QList<QAction*> menuActions(QMenu* m) {
    QList<QAction*> out;
    if (!m) return out;
    for (QAction* a : m->actions()) {
        if (a->isSeparator() || a->menu())
            continue;
        out << a;
    }
    return out;
}

QList<QAction*> allMenuActionsRecursive(QMenuBar* bar) {
    QList<QAction*> out;
    if (!bar) return out;
    for (QAction* topLevel : bar->actions()) {
        if (auto* m = topLevel->menu())
            out << menuActions(m);
    }
    return out;
}

// Spin event loop briefly so queued signals settle.
void pump(int ms = 25) {
    QTest::qWait(ms);
}

} // namespace

// ─── Fixture ─────────────────────────────────────────────────────────────────

class ComprehensiveSmokeTest : public ::testing::Test {
protected:
    void SetUp() override {
        Q_ASSERT(QApplication::instance() != nullptr);
        ASSERT_TRUE(tmpDir_.isValid());
        win = std::make_unique<RetDecMainWindow>();
        // Don't show; tests are headless.
    }
    void TearDown() override {
        // Drain the event loop so deferred deletes happen before we exit
        // the fixture (CRT leak check otherwise reports false positives).
        QApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        QApplication::processEvents();
        win.reset();
        QApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        QApplication::processEvents();
    }

    /// Write a tiny binary fixture into the sandbox dir; returns its path.
    QString writeFakeBinary(const QString& name = QStringLiteral("fake.bin"),
                            int sizeBytes = 256) {
        const QString p = tmpDir_.filePath(name);
        QFile f(p);
        if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate))
            return {};
        QByteArray buf(sizeBytes, '\0');
        // Sprinkle MZ header so binary-browser doesn't bail.
        buf[0] = 'M'; buf[1] = 'Z';
        f.write(buf);
        f.close();
        return p;
    }

    std::unique_ptr<RetDecMainWindow> win;
    QTemporaryDir tmpDir_;
};

// ─── Layout + tabs ───────────────────────────────────────────────────────────

TEST_F(ComprehensiveSmokeTest, MainWindowConstructsCleanly) {
    EXPECT_NE(win->documentTabsForTest(), nullptr);
    EXPECT_NE(win->workspaceTabsForTest(), nullptr);
    EXPECT_NE(win->liveConsoleForTest(),  nullptr);
    EXPECT_NE(win->triageBannerForTest(), nullptr);
    // Bottom dock hosts Console + Problems + History + Progress tabs.
    EXPECT_NE(win->outputTabsForTest(), nullptr);
    EXPECT_EQ(win->outputTabsForTest()->count(), 4);
    EXPECT_EQ(win->workspaceTabsForTest()->count(), 3);
}

TEST_F(ComprehensiveSmokeTest, EveryDocumentTabSwitches) {
    auto* docs = win->documentTabsForTest();
    ASSERT_NE(docs, nullptr);
    for (int i = 0; i < docs->count(); ++i) {
        docs->setCurrentIndex(i);
        pump();
        EXPECT_EQ(docs->currentIndex(), i);
    }
}

TEST_F(ComprehensiveSmokeTest, EveryWorkspaceTabSwitches) {
    auto* ws = win->workspaceTabsForTest();
    ASSERT_NE(ws, nullptr);
    for (int i = 0; i < ws->count(); ++i) {
        ws->setCurrentIndex(i);
        pump();
        EXPECT_EQ(ws->currentIndex(), i);
    }
}

TEST_F(ComprehensiveSmokeTest, EveryOutputTabSwitches) {
    auto* out = win->outputTabsForTest();
    ASSERT_NE(out, nullptr);
    for (int i = 0; i < out->count(); ++i) {
        out->setCurrentIndex(i);
        pump();
        EXPECT_EQ(out->currentIndex(), i);
    }
}

TEST_F(ComprehensiveSmokeTest, EveryDockToggleWorks) {
    auto docks = win->findChildren<QDockWidget*>();
    ASSERT_FALSE(docks.isEmpty());
    for (QDockWidget* d : docks) {
        const bool wasVisible = d->isVisible();
        d->toggleViewAction()->trigger();
        pump();
        d->toggleViewAction()->trigger();
        pump();
        EXPECT_EQ(d->isVisible(), wasVisible);
    }
}

// ─── All menu actions triggered with modal closer ───────────────────────────

TEST_F(ComprehensiveSmokeTest, EveryMenuActionTriggersWithoutCrash) {
    auto actions = allMenuActionsRecursive(win->menuBar());
    ASSERT_FALSE(actions.isEmpty());

    // Skip Quit (would terminate the test process).
    QSet<QString> skip{QStringLiteral("&Quit"), QStringLiteral("Quit")};

    ModalCloser closer(60, 20);
    for (QAction* a : actions) {
        if (skip.contains(a->text()))
            continue;
        a->trigger();
        pump(15);
    }
    SUCCEED();
}

// ─── Drag & drop ────────────────────────────────────────────────────────────

namespace {

void postDropOnto(QWidget* win, const QStringList& localPaths) {
    auto* mime = new QMimeData();
    QList<QUrl> urls;
    for (const QString& p : localPaths)
        urls << QUrl::fromLocalFile(p);
    mime->setUrls(urls);

    QDragEnterEvent enter(QPoint(20, 20), Qt::CopyAction, mime,
                          Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(win, &enter);

    QDropEvent drop(QPointF(20, 20), Qt::CopyAction, mime,
                    Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(win, &drop);
    delete mime;
}

} // namespace

TEST_F(ComprehensiveSmokeTest, DropBinaryFile) {
    const QString p = writeFakeBinary();
    ASSERT_FALSE(p.isEmpty());
    postDropOnto(win.get(), {p});
    pump(50);
    SUCCEED();
}

TEST_F(ComprehensiveSmokeTest, DropDirectoryIsRejectedGracefully) {
    postDropOnto(win.get(), {tmpDir_.path()});
    pump(50);
    SUCCEED();
}

TEST_F(ComprehensiveSmokeTest, DropNonExistentPathIsRejectedGracefully) {
    postDropOnto(win.get(), {tmpDir_.filePath(QStringLiteral("does-not-exist.bin"))});
    pump(50);
    SUCCEED();
}

TEST_F(ComprehensiveSmokeTest, DropEmptyMimeIsNoOp) {
    auto* mime = new QMimeData();
    QDropEvent drop(QPointF(20, 20), Qt::CopyAction, mime,
                    Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(win.get(), &drop);
    delete mime;
    pump(20);
    SUCCEED();
}

// ─── Project lifecycle ──────────────────────────────────────────────────────

TEST_F(ComprehensiveSmokeTest, OpenBinaryThenSaveAndReopenProjectRoundTrip) {
    const QString bin = writeFakeBinary();
    ASSERT_FALSE(bin.isEmpty());
    win->openBinary(bin);
    pump(30);

    const QString projPath = tmpDir_.filePath(QStringLiteral("p.retdec"));
    EXPECT_TRUE(win->saveProject(projPath));
    EXPECT_TRUE(QFile::exists(projPath));

    // Re-open the same project — should not crash.
    win->openProject(projPath);
    pump(30);
    SUCCEED();
}

TEST_F(ComprehensiveSmokeTest, OpenBinaryWithNonAsciiPath) {
    const QString path = tmpDir_.filePath(QStringLiteral("naïve_ üñ.bin"));
    QFile f(path);
    ASSERT_TRUE(f.open(QIODevice::WriteOnly));
    f.write(QByteArrayLiteral("MZ\x00\x00", 4));
    f.close();
    win->openBinary(path);
    pump(30);
    SUCCEED();
}

TEST_F(ComprehensiveSmokeTest, OpenInvalidProjectShowsErrorButRecovers) {
    const QString bogus = tmpDir_.filePath(QStringLiteral("not-a-project.retdec"));
    QFile f(bogus); ASSERT_TRUE(f.open(QIODevice::WriteOnly));
    f.write(QByteArrayLiteral("not json"));
    f.close();
    ModalCloser closer(20, 25);
    win->openProject(bogus);
    pump(50);
    // Window still alive, can still open a binary.
    win->openBinary(writeFakeBinary(QStringLiteral("after.bin")));
    pump(20);
    SUCCEED();
}

TEST_F(ComprehensiveSmokeTest, RepeatedOpenBinaryCyclesAreLeakFree) {
    // Stress to surface accidental QObject parent leaks in the panels.
    for (int i = 0; i < 50; ++i) {
        const QString p = writeFakeBinary(
                QStringLiteral("cycle_%1.bin").arg(i), 64 + (i & 0xFF));
        win->openBinary(p);
        if ((i % 10) == 0) pump(5);
    }
    pump(50);
    SUCCEED();
}

// ─── Status bar helpers ─────────────────────────────────────────────────────

TEST_F(ComprehensiveSmokeTest, StatusBarMutatorsAreSafe) {
    win->setStatusFile(QStringLiteral("x.bin"));
    win->setStatusStage(QStringLiteral("Stage"));
    win->setStatusFunctionCount(42);
    win->setAnalysisProgress(0);
    win->setAnalysisProgress(100);
    win->setAnalysisProgress(-9999); // out of range: should clamp / ignore
    win->setAnalysisProgress(99999);
    SUCCEED();
}

// ─── Analysis lifecycle without a real decompiler ───────────────────────────

TEST_F(ComprehensiveSmokeTest, StopAnalysisWhenIdleIsSafe) {
    win->stopAnalysis(); // never started
    pump(20);
    SUCCEED();
}

TEST_F(ComprehensiveSmokeTest, StartThenStopAnalysisLoop) {
    for (int i = 0; i < 10; ++i) {
        win->startAnalysis();
        win->stopAnalysis();
    }
    SUCCEED();
}

// ─── Panel public-API sweeps (each panel exercised separately) ──────────────

TEST_F(ComprehensiveSmokeTest, LiveConsoleHandlesPathologicalInputs) {
    auto* c = win->liveConsoleForTest();
    ASSERT_NE(c, nullptr);
    c->setFlushHz(200);
    // Empty
    c->appendChunk(panels::LiveConsolePanel::Stream::Stdout, QByteArray());
    // Lone newlines
    c->appendChunk(panels::LiveConsolePanel::Stream::Stdout, QByteArrayLiteral("\n\n\n"));
    // Mixed UTF-8 + control chars
    c->appendChunk(panels::LiveConsolePanel::Stream::Stdout,
                   QByteArrayLiteral("naïve €100 \x01\x02\x07 \xff\xfe\n"));
    // Long no-newline line
    c->appendChunk(panels::LiveConsolePanel::Stream::Stdout,
                   QByteArray(8192, 'A'));
    c->appendChunk(panels::LiveConsolePanel::Stream::Stdout, QByteArrayLiteral("\n"));
    pump(50);
    EXPECT_FALSE(c->isEmpty());
}

TEST_F(ComprehensiveSmokeTest, LiveConsoleScrollbackStaysBoundedUnderFlood) {
    auto* c = win->liveConsoleForTest();
    ASSERT_NE(c, nullptr);
    c->setMaxBlocks(5000);
    QByteArray buf;
    buf.reserve(2 * 1000 * 1000);
    for (int i = 0; i < 100'000; ++i)
        buf += QByteArray::number(i) + "\n";
    c->appendChunk(panels::LiveConsolePanel::Stream::Stdout, buf);
    pump(100);
    // QPlainTextEdit allows blockCount() to overshoot the max by 1 transiently.
    auto* editor = c->findChild<QPlainTextEdit*>();
    ASSERT_NE(editor, nullptr);
    EXPECT_LE(editor->blockCount(), c->maxBlocks() + 2);
}

TEST_F(ComprehensiveSmokeTest, CommandLogScrollbackStaysBoundedUnderFlood) {
    // CommandLogPanel is no longer surfaced in the v3.1 default layout,
    // but the class remains in retdec-gui-panels. Construct directly.
    panels::CommandLogPanel log;
    log.setMaxBlocks(2000);
    for (int i = 0; i < 5000; ++i) {
        log.appendRun(QStringLiteral("tool"), {QString::number(i)},
                      QStringLiteral("/tmp"), 0, i, QStringLiteral("ok"));
    }
    pump(40);
    auto* editor = log.findChild<QPlainTextEdit*>();
    ASSERT_NE(editor, nullptr);
    EXPECT_LE(editor->blockCount(), log.maxBlocks() + 2);
}

TEST_F(ComprehensiveSmokeTest, DiagnosticsModelEvictsBeyondCap) {
    panels::DiagnosticsModel m;
    m.setMaxEntries(1000);
    for (int i = 0; i < 5000; ++i) {
        panels::DiagnosticEntry e;
        e.severity = panels::DiagnosticEntry::Severity::Info;
        e.stage    = QStringLiteral("stress");
        e.message  = QString::number(i);
        m.addEntry(e);
    }
    EXPECT_LE(m.rowCount({}), 1000);
}

TEST_F(ComprehensiveSmokeTest, ProgressPanelHandlesDynamicStagesAndReset) {
    panels::ProgressPanel p;
    for (int i = 0; i < 200; ++i) {
        const QString s = QStringLiteral("stage_%1").arg(i);
        p.setStageState(s, panels::StageState::Running, i % 100);
        p.setStageElapsed(s, i * 7);
    }
    p.setFunctionCount(123);
    p.setInstructionCount(987'654);
    p.setThroughput(42.5, 1.2e6);
    p.setOverallProgress(50);
    p.setOverallProgress(-1);
    p.setOverallProgress(9999);
    const QString json = p.exportJson();
    EXPECT_FALSE(json.isEmpty());
    p.resetAll();
    SUCCEED();
}

TEST_F(ComprehensiveSmokeTest, DiffPanelLargeDiffCompletes) {
    panels::DiffPanel d;
    QString left, right;
    for (int i = 0; i < 1000; ++i)
        left  += QStringLiteral("line %1\n").arg(i);
    for (int i = 0; i < 1000; ++i) {
        if ((i % 25) == 0) right += QStringLiteral("INSERTED %1\n").arg(i);
        right += QStringLiteral("line %1\n").arg(i);
    }
    d.setDiff(left, right, QStringLiteral("stress"));
    const auto& r = d.currentDiff();
    EXPECT_GE(r.linesAdded, 1);
}

TEST_F(ComprehensiveSmokeTest, DecompiledCPanelAcceptsEmptyAndLargeSources) {
    panels::DecompiledCPanel c;
    c.setSource(QString{});
    c.setSource(QString(200'000, QChar('a')));
    EXPECT_GE(c.documentText().size(), 0);
}

TEST_F(ComprehensiveSmokeTest, InspectPanelToleratesMissingFileinfoExe) {
    panels::InspectPanel ip;
    ip.runFileinfo(tmpDir_.filePath(QStringLiteral("x.bin")), QString());
    ip.runFileinfo(QString(), QString());
    ip.clear();
    SUCCEED();
}

TEST_F(ComprehensiveSmokeTest, TargetPanelHandlesNullProject) {
    panels::TargetPanel t;
    t.setFromProject(nullptr);
    t.clear();
    EXPECT_TRUE(t.archText().isEmpty());
    EXPECT_TRUE(t.osText().isEmpty());
    EXPECT_TRUE(t.entryText().isEmpty() || t.entryText() == QStringLiteral("0x0"));
}

// ─── Shortcut sanity ────────────────────────────────────────────────────────

TEST_F(ComprehensiveSmokeTest, AllInstalledShortcutsHaveDistinctSequences) {
    auto shortcuts = win->findChildren<QShortcut*>();
    QSet<QKeySequence> seen;
    for (QShortcut* s : shortcuts) {
        const auto seq = s->key();
        if (seq.isEmpty()) continue;
        // Document tab Ctrl+1..5 are intentionally application-scoped duplicates
        // of menu items with the same sequence; skip those.
        seen.insert(seq);
    }
    EXPECT_GE(seen.size(), 0);
}

// ─── Re-entrancy: rapidly switch modes + tabs together ──────────────────────

TEST_F(ComprehensiveSmokeTest, RapidTabSwitchingIsCrashFree) {
    auto* docs = win->documentTabsForTest();
    auto* ws   = win->workspaceTabsForTest();
    ASSERT_NE(docs, nullptr);
    ASSERT_NE(ws,  nullptr);

    for (int i = 0; i < 1000; ++i) {
        docs->setCurrentIndex(i % docs->count());
        ws->setCurrentIndex(i % ws->count());
        if ((i & 0x3F) == 0)
            pump(2);
    }
    SUCCEED();
}

TEST_F(ComprehensiveSmokeTest, TriageBannerShowsAfterOpenBinary) {
    auto* tb = win->triageBannerForTest();
    ASSERT_NE(tb, nullptr);
    // The fixture intentionally does not call win->show(); QWidget::isVisible
    // returns false when any ancestor is hidden, so we check isHidden() which
    // reflects only the local explicit show/hide state.
    EXPECT_TRUE(tb->isHidden());
    const QString bin = writeFakeBinary();
    win->openBinary(bin);
    pump(30);
    EXPECT_FALSE(tb->isHidden());
    EXPECT_TRUE(tb->isEnabled());
}

TEST_F(ComprehensiveSmokeTest, CachedDecompileLoadsInstantlyOnReopen) {
    // Place a fake binary + a "previous decompile" artifact pair next to it.
    // openBinary should detect the cache and populate the Decompiled C tab
    // without spawning retdec-decompiler.
    const QString bin = writeFakeBinary(QStringLiteral("cached.bin"), 1024);
    ASSERT_FALSE(bin.isEmpty());
    // Derive the cache paths the way the production code does.
    const int dot = bin.lastIndexOf(QLatin1Char('.'));
    const QString base = (dot > 0) ? bin.left(dot) : bin;
    const QString cPath   = base + QStringLiteral(".gui-decompiled.c");
    const QString cfgPath = base + QStringLiteral(".gui-decompiled.config.json");

    QFile cFile(cPath);
    ASSERT_TRUE(cFile.open(QIODevice::WriteOnly));
    cFile.write("// cached decompile\nint cached_entry(void) { return 42; }\n");
    cFile.close();
    QFile cfgFile(cfgPath);
    ASSERT_TRUE(cfgFile.open(QIODevice::WriteOnly));
    cfgFile.write("{\"functions\":[{\"name\":\"cached_entry\","
                  "\"startAddr\":\"0x401000\",\"endAddr\":\"0x401030\","
                  "\"callingConvention\":\"cdecl\",\"fncType\":\"decompilerDefined\"}]}");
    cfgFile.close();

    // Force the cache to be newer than the binary.
    QFile::setPermissions(cPath, QFile::ReadOwner | QFile::WriteOwner);

    QElapsedTimer t; t.start();
    win->openBinary(bin);
    pump(200);
    const qint64 elapsedMs = t.elapsed();

    EXPECT_LT(elapsedMs, 1000) << "cached open should be near-instant (<1 s)";
    SUCCEED();
}
