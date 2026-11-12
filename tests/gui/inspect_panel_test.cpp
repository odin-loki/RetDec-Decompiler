/**
 * @file tests/gui/inspect_panel_test.cpp
 * @brief InspectPanel in a headless UiContext, with stand-in fileinfo /
 *        unpacker scripts on POSIX (includes the Qt comprehensive-smoke
 *        "tolerates missing fileinfo" check).
 * @copyright (c) 2026 Odin Loch trading as Imortek
 */

#include "code_ui_test_util.h"

#include "retdec/gui/ui/inspect_panel.h"

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <memory>

using namespace retdec::gui;
using retdec::gui::test::HeadlessUi;
namespace fs = std::filesystem;

namespace {
#if !defined(_WIN32)
fs::path script(const fs::path& dir, const std::string& name, const std::string& body)
{
	const fs::path p = dir / name;
	std::ofstream(p) << "#!/bin/sh\n" << body;
	fs::permissions(p, fs::perms::owner_all);
	return p;
}
#endif
} // namespace

class InspectPanelTest : public ::testing::Test {
protected:
	void SetUp() override
	{
		dir = retdec::gui::test::scratchDir("inspect_panel");
		std::ofstream(dir / "x.bin") << "binary";
		panel = std::make_unique<ui::InspectPanel>(h.ui, h.ui.root());
		panel->onCliToolFinished = [this](const std::string& tool, const std::vector<std::string>& args, const std::string&, int exitCode,
		                                  int64_t, const std::string& tail) {
			finished.push_back(tool + " " + std::to_string(exitCode));
			lastArgs = args;
			lastTail = tail;
		};
		panel->onProcessStarted = [this](const std::string& label, const std::vector<std::string>&, const std::string&) {
			started.push_back(label);
		};
		panel->onProcessOutput = [this](const std::string& label, const std::string& out, const std::string&) {
			outputs.push_back(label + ":" + out);
		};
		panel->onFileinfoReady = [this](const model::FileinfoInfo& info, const std::string&, const std::string& path) {
			readyFormat = info.format;
			readyPath = path;
		};
		panel->onRequestOpenBinary = [this](const std::string& p) { opened.push_back(p); };
		h.frame();
	}
	bool waitIdle() { return h.pumpUntil([this] { return !panel->busy(); }, 10'000); }

	HeadlessUi h;
	fs::path dir;
	std::unique_ptr<ui::InspectPanel> panel;
	std::vector<std::string> finished, started, outputs, opened, lastArgs;
	std::string lastTail, readyFormat, readyPath;
};

TEST_F(InspectPanelTest, InitialState)
{
	EXPECT_EQ(panel->panelTitle(), "Inspect");
	EXPECT_EQ(panel->statusText(), "No fileinfo run yet.");
	EXPECT_TRUE(h.ui.disabled(panel->unpackButton()));
	EXPECT_TRUE(h.ui.disabled(panel->openUnpackedButton()));
	EXPECT_FALSE(h.ui.disabled(panel->refreshButton()));
}

TEST_F(InspectPanelTest, ToleratesMissingFileinfoExe)
{
	panel->runFileinfo((dir / "x.bin").string(), "");
	EXPECT_EQ(panel->statusText(), "retdec-fileinfo not found (install or add to PATH).");
	EXPECT_EQ(panel->summaryCell(0, 0), "No fileinfo executable.");
	EXPECT_EQ(panel->unpackOutput(), (dir / "x-unpacked").string());
	EXPECT_FALSE(h.ui.disabled(panel->unpackButton()));
	panel->runFileinfo("", "");
	EXPECT_TRUE(h.ui.disabled(panel->unpackButton()));
	panel->clear();
	EXPECT_EQ(panel->statusText(), "No binary loaded.");
	EXPECT_EQ(panel->summaryRowCount(), 0);
}

TEST_F(InspectPanelTest, CopyHashesWithoutFileinfo)
{
	h.click(panel->copyHashesButton());
	EXPECT_EQ(panel->statusText(), "No hashes in fileinfo JSON yet.");
}

TEST_F(InspectPanelTest, DecompileModeButton)
{
	int n = 0;
	panel->onRequestDecompileMode = [&] { ++n; };
	h.click(panel->decompileModeButton());
	EXPECT_EQ(n, 1);
}

TEST_F(InspectPanelTest, UnpackerNotFound)
{
	panel->runFileinfo((dir / "x.bin").string(), "");
	panel->runUnpacker();
	EXPECT_EQ(panel->statusText(), "retdec-unpacker not found.");
}

TEST_F(InspectPanelTest, SpawnErrorIsReported)
{
	panel->runFileinfo((dir / "x.bin").string(), (dir / "no-such-fileinfo").string());
	ASSERT_TRUE(waitIdle());
	h.frame(3);
	EXPECT_NE(panel->statusText().find("fileinfo error:"), std::string::npos);
	EXPECT_EQ(panel->summaryCell(0, 0), "Process error.");
	ASSERT_EQ(finished.size(), 1u);
	EXPECT_EQ(finished[0], "retdec-fileinfo -1");
}

#if !defined(_WIN32)
TEST_F(InspectPanelTest, RunsFileinfoAndShowsSummaryAndJson)
{
	const auto fi = script(dir, "fileinfo",
	                       "echo '{\"fileFormat\":\"ELF\",\"architecture\":\"x86\",\"sha256\":\"aa11\",\"md5\":\"bb22\",\"crc32\":\"cc33\",\"fileSize\":6}'\n");
	panel->runFileinfo((dir / "x.bin").string(), fi.string());
	EXPECT_EQ(panel->statusText(), "Running retdec-fileinfo\xE2\x80\xA6");
	EXPECT_TRUE(h.ui.disabled(panel->refreshButton()));
	ASSERT_EQ(started.size(), 1u);
	EXPECT_EQ(started[0], "retdec-fileinfo");
	ASSERT_TRUE(waitIdle());
	h.frame(3);
	EXPECT_EQ(panel->statusText(), "fileinfo OK \xE2\x80\x94 see Summary / JSON tabs.");
	EXPECT_FALSE(h.ui.disabled(panel->refreshButton()));
	EXPECT_EQ(panel->summaryCell(0, 0), "sha256");
	EXPECT_EQ(panel->summaryCell(0, 1), "aa11");
	EXPECT_EQ(panel->summaryCell(3, 0), "architecture");
	EXPECT_NE(panel->rawText().find("  \"fileFormat\": \"ELF\""), std::string::npos);
	ASSERT_EQ(finished.size(), 1u);
	EXPECT_EQ(finished[0], "retdec-fileinfo 0");
	EXPECT_EQ(lastArgs, model::fileinfoArgs((dir / "x.bin").string()));
	EXPECT_EQ(readyFormat, "ELF");
	EXPECT_EQ(readyPath, (dir / "x.bin").string());
	ASSERT_EQ(outputs.size(), 1u);
	EXPECT_EQ(outputs[0].substr(0, 17), "retdec-fileinfo:{");
	ASSERT_TRUE(panel->fileinfo().has_value());
	h.click(panel->copyHashesButton());
	EXPECT_EQ(h.clipboard(), "SHA-256: aa11\nMD5: bb22\nCRC32: cc33");
	EXPECT_EQ(panel->statusText(), "Copied file hashes.");
	// Tabs: switch to JSON and back.
	h.ui.setValue(fuse::ui::widgets::tabWidgetBar(h.ui, panel->tabs()), 1.f, true);
	EXPECT_EQ(panel->currentTab(), 1);
	h.click(panel->refreshButton()); // re-runs and returns to Summary
	EXPECT_EQ(panel->currentTab(), 0);
	ASSERT_TRUE(waitIdle());
}

TEST_F(InspectPanelTest, NonZeroExitAndBadJson)
{
	const auto bad = script(dir, "fileinfo-bad", "echo 'oops'\nexit 2\n");
	panel->runFileinfo((dir / "x.bin").string(), bad.string());
	ASSERT_TRUE(waitIdle());
	h.frame(3);
	EXPECT_EQ(panel->statusText(), "fileinfo failed (exit 2)");
	EXPECT_EQ(panel->summaryCell(0, 0), "Non-zero exit. See JSON tab.");
	EXPECT_EQ(panel->rawText(), "oops\n");
	const auto silent = script(dir, "fileinfo-silent", "exit 1\n");
	panel->runFileinfo((dir / "x.bin").string(), silent.string());
	ASSERT_TRUE(waitIdle());
	h.frame(3);
	EXPECT_EQ(panel->rawText(), "(no output)");
	const auto junk = script(dir, "fileinfo-junk", "echo 'not json'\n");
	panel->runFileinfo((dir / "x.bin").string(), junk.string());
	ASSERT_TRUE(waitIdle());
	h.frame(3);
	EXPECT_NE(panel->summaryCell(0, 0).find("Could not parse JSON"), std::string::npos);
	EXPECT_EQ(panel->rawText(), "not json\n");
	EXPECT_FALSE(panel->fileinfo().has_value());
}

TEST_F(InspectPanelTest, UnpackerRunsAndOpensResult)
{
	const auto un = script(dir, "unpacker", "out=\"\"\nwhile [ $# -gt 0 ]; do if [ \"$1\" = -o ]; then out=\"$2\"; fi; shift; done\n"
	                                         "echo packed > \"$out\"\necho done\n");
	panel->runFileinfo((dir / "x.bin").string(), "");
	panel->setUnpackerExecutable(un.string());
	panel->setAutoOpen(true);
	h.frame();
	h.click(panel->bruteCheck());
	h.click(panel->unpackButton());
	EXPECT_EQ(panel->statusText(), "Running retdec-unpacker\xE2\x80\xA6");
	ASSERT_TRUE(waitIdle());
	h.frame(3);
	const std::string out = (dir / "x-unpacked").string();
	EXPECT_EQ(panel->statusText(), "Unpacker finished (exit 0). Output: " + out);
	ASSERT_EQ(finished.size(), 1u);
	EXPECT_EQ(finished[0], "retdec-unpacker 0");
	EXPECT_EQ(lastArgs, model::unpackerArgs((dir / "x.bin").string(), out, true));
	EXPECT_EQ(lastTail, "done\n");
	EXPECT_FALSE(h.ui.disabled(panel->openUnpackedButton()));
	ASSERT_EQ(opened.size(), 1u); // "Open when done"
	EXPECT_EQ(opened[0], out);
	h.click(panel->openUnpackedButton());
	EXPECT_EQ(opened.size(), 2u);
}

TEST_F(InspectPanelTest, UnpackerFailure)
{
	const auto un = script(dir, "unpacker-fail", "echo 'not packed' >&2\nexit 1\n");
	panel->runFileinfo((dir / "x.bin").string(), "");
	panel->setUnpackerExecutable(un.string());
	panel->setUnpackOutput((dir / "never").string());
	panel->runUnpacker();
	ASSERT_TRUE(waitIdle());
	h.frame(3);
	EXPECT_EQ(panel->statusText(), "Unpacker exit 1 \xE2\x80\x94 see Command log / Diagnostics.");
	EXPECT_EQ(lastTail, "not packed\n");
	EXPECT_TRUE(h.ui.disabled(panel->openUnpackedButton()));
	EXPECT_TRUE(opened.empty());
}
#endif
