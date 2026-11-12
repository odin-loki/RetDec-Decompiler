/**
 * @file tests/gui/live_console_panel_test.cpp
 * @brief LiveConsolePanel (port of the Qt live_console_panel_test.cpp and the
 *        console parts of comprehensive_smoke_test.cpp): streaming contract
 *        end-to-end with a real child process, capping, ANSI, banners.
 * @copyright (c) 2026 Odin Loch trading as Imortek
 */

#include "code_ui_test_util.h"

#include "retdec/gui/ui/live_console_panel.h"

#include <forge/os/process.hpp>

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <format>
#include <fstream>
#include <memory>
#include <thread>

using namespace retdec::gui;
using retdec::gui::test::HeadlessUi;
using ui::LiveConsolePanel;
using Stream = LiveConsolePanel::Stream;

class LiveConsolePanelTest : public ::testing::Test {
protected:
	void SetUp() override
	{
		panel = std::make_unique<LiveConsolePanel>(h.ui, h.ui.root());
		panel->setFlushHz(200);
		h.frame();
	}
	void TearDown() override
	{
		h.frame();
		panel.reset();
	}
	HeadlessUi h;
	std::unique_ptr<LiveConsolePanel> panel;
};

// ─── Construction ────────────────────────────────────────────────────────────

TEST_F(LiveConsolePanelTest, ConstructsAndExposesEditor)
{
	ASSERT_NE(panel.get(), nullptr);
	EXPECT_TRUE(h.ui.alive(panel->view().node()));
	EXPECT_EQ(panel->panelTitle(), "Console");
	EXPECT_TRUE(panel->isEmpty());
	EXPECT_EQ(panel->statusText(), "(idle)");
}

TEST_F(LiveConsolePanelTest, DefaultMaxBlocksAreReasonable)
{
	EXPECT_GE(panel->maxBlocks(), 1000);
}

TEST_F(LiveConsolePanelTest, SetMaxBlocksClampsToMin)
{
	panel->setMaxBlocks(10);
	EXPECT_GE(panel->maxBlocks(), 1000);
	panel->setMaxBlocks(7777);
	EXPECT_EQ(panel->maxBlocks(), 7777);
}

TEST_F(LiveConsolePanelTest, FlushRateAndCapClamp)
{
	panel->setFlushHz(1);
	EXPECT_EQ(panel->flushHz(), 2);
	panel->setFlushHz(1000);
	EXPECT_EQ(panel->flushHz(), 250);
	panel->setMaxBytesPerFlush(10);
	EXPECT_EQ(panel->maxBytesPerFlush(), 4 * 1024);
}

// ─── appendChunk / appendLine ────────────────────────────────────────────────

TEST_F(LiveConsolePanelTest, AppendLineAppearsImmediately)
{
	panel->appendLine(Stream::Stdout, "hello");
	h.frame();
	EXPECT_NE(panel->view().text().find("hello"), std::string::npos);
	EXPECT_FALSE(panel->isEmpty());
}

TEST_F(LiveConsolePanelTest, AppendChunkSplitsByNewline)
{
	panel->appendChunk(Stream::Stdout, "line one\nline two\n");
	h.frame();
	EXPECT_EQ(panel->view().line(0), "line one");
	EXPECT_EQ(panel->view().line(1), "line two");
}

TEST_F(LiveConsolePanelTest, PartialLinesJoin)
{
	panel->appendChunk(Stream::Stdout, "first ha");
	panel->appendChunk(Stream::Stdout, "lf\nsecond\n");
	EXPECT_EQ(panel->view().line(0), "first half");
	EXPECT_EQ(panel->view().line(1), "second");
}

TEST_F(LiveConsolePanelTest, StderrTextAppearsWithoutPrefix)
{
	panel->appendChunk(Stream::Stderr, "boom\n");
	h.frame();
	const std::string text = panel->text();
	EXPECT_NE(text.find("boom"), std::string::npos);
	EXPECT_EQ(text.find("[stderr]"), std::string::npos);
}

TEST_F(LiveConsolePanelTest, ClearEmptiesPanel)
{
	panel->appendLine(Stream::Stdout, "noise");
	h.frame();
	EXPECT_FALSE(panel->isEmpty());
	panel->clear();
	h.frame();
	EXPECT_TRUE(panel->isEmpty());
	EXPECT_EQ(panel->statusText(), "(idle)");
}

TEST_F(LiveConsolePanelTest, AnsiEscapesAreStripped)
{
	panel->appendChunk(Stream::Stdout, "\x1b[31mred\x1b[0m text\n");
	h.frame();
	const std::string text = panel->text();
	EXPECT_EQ(text.find("\x1b["), std::string::npos);
	EXPECT_NE(text.find("red text"), std::string::npos);
}

TEST_F(LiveConsolePanelTest, BannerAndFooterAppear)
{
	panel->appendBanner("retdec-foo", {"--bar", "baz"}, "/tmp");
	EXPECT_EQ(panel->statusText(), "running: retdec-foo");
	panel->appendFooter("retdec-foo", 0, 42);
	h.frame();
	const std::string text = panel->text();
	EXPECT_NE(text.find("==> "), std::string::npos);
	EXPECT_NE(text.find("retdec-foo --bar baz"), std::string::npos);
	EXPECT_NE(text.find("cwd: /tmp"), std::string::npos);
	EXPECT_NE(text.find("<== retdec-foo finished"), std::string::npos);
	EXPECT_NE(text.find("exit=0"), std::string::npos);
	EXPECT_NE(text.find("time=42 ms"), std::string::npos);
	EXPECT_EQ(panel->statusText(), "idle (last: retdec-foo, exit=0)");
}

TEST_F(LiveConsolePanelTest, LinesAreColouredByContent)
{
	panel->appendBanner("tool", {}, "");
	panel->appendLine(Stream::Stdout, "[INFO] processing error handler");
	panel->appendLine(Stream::Stdout, "Warning: something odd");
	panel->appendLine(Stream::Stderr, "fatal: cannot open");
	panel->appendFooter("tool", 1, 5);
	const auto& b = panel->buffer();
	EXPECT_EQ(b.kind(0), model::ConsoleLineKind::Banner);
	EXPECT_EQ(b.kind(1), model::ConsoleLineKind::Plain);
	EXPECT_EQ(b.kind(2), model::ConsoleLineKind::Warning);
	EXPECT_EQ(b.kind(3), model::ConsoleLineKind::Error);
	EXPECT_EQ(b.kind(4), model::ConsoleLineKind::Footer);
}

// ─── Live sources (Qt attachProcess) ─────────────────────────────────────────

TEST_F(LiveConsolePanelTest, AttachedProcessOutputAppearsLive)
{
	const uint64_t src = panel->attachSource("echo-test");
	auto feed = panel->feed();
	// A real child process; its stdout is read on a worker thread and pushed
	// into the feed while the UI keeps running frames.
	std::thread reader([feed, src] {
#if defined(_WIN32)
		auto c = forge::os::child::start({"cmd.exe", "/c", "echo first& echo second"}, true);
#else
		auto c = forge::os::child::start({"/bin/sh", "-c", "printf 'first\\nsecond\\n'"}, true);
#endif
		if (!c) return;
		for (;;)
		{
			std::string s = c->read();
			if (s.empty()) break;
			feed->push(src, model::ConsoleStream::Stdout, s);
		}
		c->wait();
	});
	EXPECT_TRUE(h.pumpUntil([&] { return panel->text().find("second") != std::string::npos; }, 5000));
	reader.join();
	const std::string text = panel->text();
	EXPECT_NE(text.find("first"), std::string::npos);
	EXPECT_NE(text.find("second"), std::string::npos);
	panel->detachSource(src);
}

TEST_F(LiveConsolePanelTest, DetachDrainsPendingOutput)
{
	const uint64_t src = panel->attachSource("late");
	panel->pushOutput(src, Stream::Stdout, "tail line\n");
	panel->detachSource(src); // before any flush tick
	EXPECT_NE(panel->text().find("tail line"), std::string::npos);
	EXPECT_FALSE(panel->feed()->attached(src));
}

TEST_F(LiveConsolePanelTest, DetachProcessIsSafeAfterDestroy)
{
	uint64_t src = panel->attachSource("ephemeral");
	auto feed = panel->feed();
	panel.reset(); // producers keep the feed alive; pushing is harmless
	feed->push(src, model::ConsoleStream::Stdout, "late\n");
	h.frame();
	SUCCEED();
}

TEST_F(LiveConsolePanelTest, ScrollbackIsBounded)
{
	panel->setMaxBlocks(2000);
	std::string buf;
	for (int i = 0; i < 6000; ++i)
		buf += std::to_string(i) + "\n";
	panel->appendChunk(Stream::Stdout, buf);
	h.frame();
	EXPECT_LE(panel->blockCount(), 2001);
	EXPECT_EQ(panel->view().line(panel->blockCount() - 2), "5999");
}

TEST_F(LiveConsolePanelTest, AutoScrollFollowsTailAndCanBeTurnedOff)
{
	std::string buf;
	for (int i = 0; i < 500; ++i)
		buf += "x" + std::to_string(i) + "\n";
	panel->appendChunk(Stream::Stdout, buf);
	h.frame();
	ui::CodeView& v = panel->view();
	EXPECT_EQ(v.firstVisibleLine(), v.scrollMax());
	// Untick Auto-scroll like a user; scroll back; new output keeps the position.
	h.click(panel->autoScrollCheck());
	EXPECT_FALSE(panel->autoScroll());
	v.setFirstVisibleLine(10);
	panel->appendChunk(Stream::Stdout, "more\nmore\n");
	h.frame();
	EXPECT_EQ(v.firstVisibleLine(), 10);
	panel->setAutoScroll(true);
	h.frame();
	EXPECT_EQ(v.firstVisibleLine(), v.scrollMax());
}

// ─── Performance: per-flush byte cap ─────────────────────────────────────────

TEST_F(LiveConsolePanelTest, LargeFloodIsChunkedAcrossFlushes)
{
	panel->setMaxBytesPerFlush(64 * 1024);
	const uint64_t src = panel->attachSource("flood");
	std::string buf;
	buf.reserve(2 * 1024 * 1024);
	for (int i = 0; i < 30'000; ++i)
		buf += "the quick brown fox jumps over the lazy dog\n";
	panel->pushOutput(src, Stream::Stdout, buf);
	// One tick inserts at most the cap.
	panel->flush();
	EXPECT_LE(panel->text().size(), 64u * 1024u + 1u);
	EXPECT_GT(panel->feed()->pendingBytes(), 0u);
	// The rest drains over the following frames without stalling any of them.
	const auto start = std::chrono::steady_clock::now();
	EXPECT_TRUE(h.pumpUntil([&] { return panel->feed()->pendingBytes() == 0; }, 10'000));
	const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count();
	EXPECT_LT(ms, 10'000);
	EXPECT_GT(panel->blockCount(), 1000);
	panel->detachSource(src);
}

TEST_F(LiveConsolePanelTest, DirectAppendOfTwoMegabytesIsFast)
{
	std::string buf;
	buf.reserve(2 * 1024 * 1024);
	for (int i = 0; i < 30'000; ++i)
		buf += "the quick brown fox jumps over the lazy dog\n";
	const auto start = std::chrono::steady_clock::now();
	panel->appendChunk(Stream::Stdout, buf);
	h.frame();
	const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count();
	EXPECT_LT(ms, 1500) << "appendChunk too slow for 2MiB buffer";
	EXPECT_GT(panel->blockCount(), 1000);
}

TEST_F(LiveConsolePanelTest, PerCallStaysUnderFrameBudgetForRealisticChunks)
{
	std::string buf;
	for (int i = 0; buf.size() < 16 * 1024; ++i)
		buf += std::format("[INFO] processing function sub_{:x}\n", 0x401000 + i);
	long long worstUs = 0;
	for (int trial = 0; trial < 20; ++trial)
	{
		panel->clear();
		const auto t = std::chrono::steady_clock::now();
		panel->appendChunk(Stream::Stdout, buf);
		worstUs = std::max<long long>(worstUs, std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - t).count());
	}
	EXPECT_LT(worstUs, 16'000) << "16 KiB insert exceeded 16 ms frame budget (worst=" << worstUs << " us)";
}

// ─── Comprehensive smoke: pathological input ─────────────────────────────────

TEST_F(LiveConsolePanelTest, HandlesPathologicalInputs)
{
	panel->appendChunk(Stream::Stdout, std::string());
	panel->appendChunk(Stream::Stdout, "\n\n\n");
	panel->appendChunk(Stream::Stdout, "na\xC3\xAFve \xE2\x82\xAC" "100 \x01\x02\x07 \xff\xfe\n");
	panel->appendChunk(Stream::Stdout, std::string(8192, 'A'));
	panel->appendChunk(Stream::Stdout, "\n");
	h.frame(3);
	EXPECT_FALSE(panel->isEmpty());
	// Invalid bytes were repaired, valid UTF-8 kept.
	EXPECT_NE(panel->text().find("na\xC3\xAFve"), std::string::npos);
	EXPECT_NE(panel->text().find("\xEF\xBF\xBD"), std::string::npos);
}

TEST_F(LiveConsolePanelTest, ScrollbackStaysBoundedUnderFlood)
{
	panel->setMaxBlocks(5000);
	std::string buf;
	buf.reserve(2 * 1000 * 1000);
	for (int i = 0; i < 100'000; ++i)
		buf += std::to_string(i) + "\n";
	panel->appendChunk(Stream::Stdout, buf);
	h.frame(5);
	EXPECT_LE(panel->blockCount(), panel->maxBlocks() + 2);
}

// ─── Copy / save ─────────────────────────────────────────────────────────────

TEST_F(LiveConsolePanelTest, CopyAllButtonAndShortcut)
{
	panel->appendLine(Stream::Stdout, "copy me");
	h.click(panel->copyButton());
	EXPECT_EQ(h.clipboard(), "copy me\n");
	h.ui.setClipboardText("");
	h.ui.setFocus(panel->view().node());
	h.key(fuse::ui::Key::C, fuse::ui::kModCtrl | fuse::ui::kModShift);
	EXPECT_EQ(h.clipboard(), "copy me\n");
}

TEST_F(LiveConsolePanelTest, ClearButtonClears)
{
	panel->appendLine(Stream::Stdout, "noise");
	h.click(panel->clearButton());
	EXPECT_TRUE(panel->isEmpty());
}

TEST_F(LiveConsolePanelTest, SaveAsWritesFileAndReportsErrors)
{
	const auto dir = retdec::gui::test::scratchDir("console_save");
	panel->appendLine(Stream::Stdout, "saved line");
	ASSERT_TRUE(panel->saveAs(dir / "out.txt"));
	std::ifstream in(dir / "out.txt");
	std::string content((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
	EXPECT_EQ(content, "saved line\n");
	EXPECT_TRUE(panel->lastError().empty());
	EXPECT_FALSE(panel->saveAs(dir / "no" / "such" / "dir" / "x.txt"));
	EXPECT_FALSE(panel->lastError().empty());
}

TEST_F(LiveConsolePanelTest, SaveButtonOpensDialog)
{
	h.click(panel->saveButton());
	EXPECT_TRUE(h.ui.alive(panel->saveDialog()));
}
