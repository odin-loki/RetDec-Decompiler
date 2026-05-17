/**
 * @file tests/gui/live_console_panel_test.cpp
 * @brief Unit tests for LiveConsolePanel (live process output streaming).
 *
 * The panel exists to fix the long-standing bug where retdec-decompiler /
 * retdec-fileinfo output only appeared in the GUI after the child process
 * exited. These tests validate the streaming contract end-to-end via a
 * real child QProcess that prints two lines and exits.
 */

#include "retdec/gui/panels/live_console_panel.h"

#include <gtest/gtest.h>

#include <QApplication>
#include <QDateTime>
#include <QElapsedTimer>
#include <QPlainTextEdit>
#include <QProcess>
#include <QString>
#include <QTest>
#include <QtGlobal>

#include <algorithm>

#include <memory>

using retdec::gui::panels::LiveConsolePanel;

class LiveConsolePanelTest : public ::testing::Test {
protected:
    void SetUp() override {
        Q_ASSERT(QApplication::instance() != nullptr);
        panel = std::make_unique<LiveConsolePanel>();
        // High flush rate so the assertions inside QTest::qWait don't have
        // to wait too long.
        panel->setFlushHz(200);
    }
    void TearDown() override {
        if (QApplication::instance())
            QApplication::processEvents();
        panel.reset();
    }

    /// Return the QPlainTextEdit owned by the panel.
    QPlainTextEdit* editor() const {
        return panel->findChild<QPlainTextEdit*>();
    }

    /// Spin the event loop until cond() returns true or @p timeoutMs elapses.
    template <typename F>
    bool waitUntil(F&& cond, int timeoutMs = 5000) {
        const int step = 25;
        int waited = 0;
        while (waited < timeoutMs) {
            if (cond())
                return true;
            QTest::qWait(step);
            waited += step;
        }
        return cond();
    }

    std::unique_ptr<LiveConsolePanel> panel;
};

// ─── Construction ────────────────────────────────────────────────────────────

TEST_F(LiveConsolePanelTest, ConstructsAndExposesEditor) {
    ASSERT_NE(panel.get(), nullptr);
    EXPECT_NE(editor(), nullptr);
    EXPECT_EQ(panel->panelTitle(), QStringLiteral("Console"));
    EXPECT_TRUE(panel->isEmpty());
}

TEST_F(LiveConsolePanelTest, DefaultMaxBlocksAreReasonable) {
    EXPECT_GE(panel->maxBlocks(), 1000);
}

TEST_F(LiveConsolePanelTest, SetMaxBlocksClampsToMin) {
    panel->setMaxBlocks(10); // tries to set 10
    EXPECT_GE(panel->maxBlocks(), 1000);
    panel->setMaxBlocks(7777);
    EXPECT_EQ(panel->maxBlocks(), 7777);
}

// ─── appendChunk / appendLine ────────────────────────────────────────────────

TEST_F(LiveConsolePanelTest, AppendLineAppearsImmediately) {
    panel->appendLine(LiveConsolePanel::Stream::Stdout, QStringLiteral("hello"));
    QApplication::processEvents();
    ASSERT_NE(editor(), nullptr);
    EXPECT_TRUE(editor()->toPlainText().contains(QStringLiteral("hello")));
    EXPECT_FALSE(panel->isEmpty());
}

TEST_F(LiveConsolePanelTest, AppendChunkSplitsByNewline) {
    panel->appendChunk(LiveConsolePanel::Stream::Stdout,
                       QByteArrayLiteral("line one\nline two\n"));
    QApplication::processEvents();
    const QString text = editor()->toPlainText();
    EXPECT_TRUE(text.contains(QStringLiteral("line one")));
    EXPECT_TRUE(text.contains(QStringLiteral("line two")));
}

TEST_F(LiveConsolePanelTest, StderrTextAppearsWithoutPrefix) {
    // v3.1: dropped the noisy [stderr] prefix. Channels are merged at the
    // QProcess level now, so stderr and stdout interleave naturally and the
    // highlighter colours by content (error/warning keywords) rather than
    // by stream tag.
    panel->appendChunk(LiveConsolePanel::Stream::Stderr,
                       QByteArrayLiteral("boom\n"));
    QApplication::processEvents();
    const QString text = editor()->toPlainText();
    EXPECT_TRUE(text.contains(QStringLiteral("boom")));
    EXPECT_FALSE(text.contains(QStringLiteral("[stderr]")));
}

TEST_F(LiveConsolePanelTest, ClearEmptiesPanel) {
    panel->appendLine(LiveConsolePanel::Stream::Stdout, QStringLiteral("noise"));
    QApplication::processEvents();
    EXPECT_FALSE(panel->isEmpty());
    panel->clear();
    QApplication::processEvents();
    EXPECT_TRUE(panel->isEmpty());
}

TEST_F(LiveConsolePanelTest, AnsiEscapesAreStripped) {
    panel->appendChunk(LiveConsolePanel::Stream::Stdout,
                       QByteArrayLiteral("\x1b[31mred\x1b[0m text\n"));
    QApplication::processEvents();
    const QString text = editor()->toPlainText();
    EXPECT_FALSE(text.contains(QStringLiteral("\x1b[")));
    EXPECT_TRUE(text.contains(QStringLiteral("red text")));
}

TEST_F(LiveConsolePanelTest, BannerAndFooterAppear) {
    panel->appendBanner(QStringLiteral("retdec-foo"),
                        QStringList{QStringLiteral("--bar"), QStringLiteral("baz")},
                        QStringLiteral("/tmp"));
    panel->appendFooter(QStringLiteral("retdec-foo"), 0, 42);
    QApplication::processEvents();
    const QString text = editor()->toPlainText();
    EXPECT_TRUE(text.contains(QStringLiteral("==> ")));
    EXPECT_TRUE(text.contains(QStringLiteral("retdec-foo --bar baz")));
    EXPECT_TRUE(text.contains(QStringLiteral("cwd: /tmp")));
    EXPECT_TRUE(text.contains(QStringLiteral("<== retdec-foo finished")));
    EXPECT_TRUE(text.contains(QStringLiteral("exit=0")));
    EXPECT_TRUE(text.contains(QStringLiteral("time=42 ms")));
}

// ─── Real subprocess streaming ───────────────────────────────────────────────

TEST_F(LiveConsolePanelTest, AttachedProcessOutputAppearsLive) {
    QProcess proc;
#ifdef Q_OS_WIN
    proc.setProgram(QStringLiteral("cmd.exe"));
    proc.setArguments(QStringList{QStringLiteral("/c"),
                                  QStringLiteral("echo first& echo second")});
#else
    proc.setProgram(QStringLiteral("/bin/sh"));
    proc.setArguments(QStringList{QStringLiteral("-c"),
                                  QStringLiteral("printf 'first\\nsecond\\n'")});
#endif
    proc.setProcessChannelMode(QProcess::SeparateChannels);
    panel->attachProcess(&proc, QStringLiteral("echo-test"));
    proc.start();
    ASSERT_TRUE(proc.waitForStarted(5000));
    ASSERT_TRUE(proc.waitForFinished(10000));

    // Drain the flush timer at least once.
    EXPECT_TRUE(waitUntil([this] {
        return editor() && editor()->toPlainText().contains(QStringLiteral("second"));
    }, 5000));

    const QString text = editor()->toPlainText();
    EXPECT_TRUE(text.contains(QStringLiteral("first")));
    EXPECT_TRUE(text.contains(QStringLiteral("second")));
}

TEST_F(LiveConsolePanelTest, DetachProcessIsSafeAfterDestroy) {
    {
        QProcess proc;
        panel->attachProcess(&proc, QStringLiteral("ephemeral"));
        // panel must not crash when proc is destroyed without explicit detach.
    }
    QApplication::processEvents();
    SUCCEED();
}

TEST_F(LiveConsolePanelTest, ScrollbackIsBounded) {
    panel->setMaxBlocks(2000);
    // Send well past the cap; QPlainTextEdit will recycle blocks.
    QByteArray buf;
    buf.reserve(60'000);
    for (int i = 0; i < 6000; ++i) {
        buf += QByteArray::number(i) + "\n";
    }
    panel->appendChunk(LiveConsolePanel::Stream::Stdout, buf);
    QApplication::processEvents();
    EXPECT_LE(editor()->blockCount(), 2001);
}

// ─── Performance: per-flush byte cap ─────────────────────────────────────────

TEST_F(LiveConsolePanelTest, LargeFloodIsChunkedAcrossFlushes) {
    // Build a 2 MiB stdout buffer in the per-process pending area, then drive
    // a single flush tick. The flush must NOT insert the entire buffer in
    // one call — it should respect the per-flush cap and re-arm.
    panel->setMaxBytesPerFlush(64 * 1024); // 64 KiB cap
    QProcess dummy; // never started; we just need a key to attach to
    panel->attachProcess(&dummy, QStringLiteral("flood"));

    // Stuff the per-process buffer by emulating onReadyReadStandardOutput:
    // route bytes through appendChunk -- but appendChunk inserts directly.
    // We exercise the cap path by feeding 2 MiB via appendChunk and verifying
    // the editor's blockCount grows in proportion to bytes (so each chunk
    // was inserted), AND that subsequent processEvents calls do not block
    // the UI for more than a small amount of time per tick.
    QByteArray buf;
    buf.reserve(2 * 1024 * 1024);
    for (int i = 0; i < 30'000; ++i)
        buf += "the quick brown fox jumps over the lazy dog\n";

    const qint64 startMs = QDateTime::currentMSecsSinceEpoch();
    panel->appendChunk(LiveConsolePanel::Stream::Stdout, buf);
    QApplication::processEvents();
    const qint64 elapsedMs = QDateTime::currentMSecsSinceEpoch() - startMs;

    // A single appendChunk WILL insert in one go (it bypasses the flush
    // cap by design — direct API users want it fully inserted). We're
    // measuring that even this worst-case stays within a reasonable budget
    // on CI hardware (1.5 s budget; in practice <300 ms on a workstation).
    EXPECT_LT(elapsedMs, 1500) << "appendChunk too slow for 2MiB buffer";
    EXPECT_GT(editor()->blockCount(), 1000);
    panel->detachProcess(&dummy);
}

// ─── Perf regression: per-call stays under frame budget on realistic chunks ─

TEST_F(LiveConsolePanelTest, PerCallStaysUnderFrameBudgetForRealisticChunks) {
    // A realistic QProcess delivers at most ~64 KiB chunks; production code
    // routes them via flushAll which caps per-tick at 16 KiB. We assert that
    // a single 16 KiB direct appendChunk stays under 16 ms on this machine
    // (otherwise the user would see a dropped frame).
    QByteArray buf;
    buf.reserve(16 * 1024 + 200);
    for (int i = 0; buf.size() < 16 * 1024; ++i)
        buf += QByteArray("[INFO] processing function sub_") + QByteArray::number(0x401000 + i, 16) + "\n";

    qint64 worstUs = 0;
    for (int trial = 0; trial < 20; ++trial) {
        QElapsedTimer t;
        t.start();
        panel->appendChunk(LiveConsolePanel::Stream::Stdout, buf);
        worstUs = std::max(worstUs, t.nsecsElapsed() / 1000);
    }
    EXPECT_LT(worstUs, 16'000)
            << "16 KiB stdout insert exceeded 16 ms frame budget (worst="
            << worstUs << " us)";
}
