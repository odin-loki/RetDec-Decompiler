/**
 * @file tests/gui/command_log_panel_test.cpp
 * @brief CommandLogPanel (the bottom dock's "History" tab) in a headless
 *        UiContext, including the Qt comprehensive-smoke flood check.
 * @copyright (c) 2026 Odin Loch trading as Imortek
 */

#include "code_ui_test_util.h"

#include "retdec/gui/ui/command_log_panel.h"

#include <gtest/gtest.h>

#include <fstream>
#include <memory>

using namespace retdec::gui;
using retdec::gui::test::HeadlessUi;

class CommandLogPanelTest : public ::testing::Test {
protected:
	void SetUp() override
	{
		log = std::make_unique<ui::CommandLogPanel>(h.ui, h.ui.root());
		h.frame();
	}
	HeadlessUi h;
	std::unique_ptr<ui::CommandLogPanel> log;
};

TEST_F(CommandLogPanelTest, AppendRunFormatsRecord)
{
	EXPECT_EQ(log->panelTitle(), "Command log");
	EXPECT_TRUE(log->isEmpty());
	log->appendRun("retdec-fileinfo", {"--json", "/a b"}, "/opt/retdec/bin", 0, 37, "{ }");
	const std::string t = log->text();
	EXPECT_EQ(t.front(), '[');
	EXPECT_NE(
		t.find("] retdec-fileinfo\n  cwd: /opt/retdec/bin\n  exit: 0  time: 37 ms\n  cmd: --json /a b\n"),
		std::string::npos);
	EXPECT_NE(t.find("  --- output (tail) ---\n{ }\n"), std::string::npos);
	log->appendRun("retdec-unpacker", {}, "/w", 2, 5, "   ");
	const std::string t2 = log->text();
	EXPECT_NE(t2.find("retdec-unpacker"), std::string::npos);
	EXPECT_EQ(t2.find("output (tail)", t2.find("retdec-unpacker")), std::string::npos);
}

TEST_F(CommandLogPanelTest, ScrollbackStaysBoundedUnderFlood)
{
	log->setMaxBlocks(2000);
	for (int i = 0; i < 5000; ++i)
		log->appendRun("tool", {std::to_string(i)}, "/tmp", 0, i, "ok");
	h.frame(3);
	EXPECT_LE(log->blockCount(), log->maxBlocks() + 2);
	log->setMaxBlocks(5);
	EXPECT_EQ(log->maxBlocks(), 100);
}

TEST_F(CommandLogPanelTest, CopyClearSave)
{
	log->appendRun("tool", {"x"}, "/w", 0, 1, "");
	h.click(log->copyButton());
	EXPECT_EQ(h.clipboard(), log->text());
	h.ui.setClipboardText("");
	h.ui.setFocus(log->view().node());
	h.key(fuse::ui::Key::C, fuse::ui::kModCtrl | fuse::ui::kModShift);
	EXPECT_EQ(h.clipboard(), log->text());
	const auto dir = retdec::gui::test::scratchDir("command_log");
	ASSERT_TRUE(log->saveTo(dir / "log.txt"));
	std::ifstream in(dir / "log.txt");
	std::string content((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
	EXPECT_EQ(content, log->text());
	std::string err;
	log->onStatusMessage = [&](const std::string& m, int) { err = m; };
	EXPECT_FALSE(log->saveTo(dir / "nope" / "log.txt"));
	EXPECT_NE(err.find("Save failed"), std::string::npos);
	h.click(log->clearButton());
	EXPECT_TRUE(log->isEmpty());
}
