/**
 * @file tests/gui/binary_browser_panel_test.cpp
 * @brief BinaryBrowserPanel in a headless UiContext: fileinfo sections, hex
 *        dump of the selection, activation, section context menu.
 * @copyright (c) 2026 Odin Loch trading as Imortek
 */

#include "code_ui_test_util.h"

#include "retdec/gui/ui/binary_browser_panel.h"

#include <gtest/gtest.h>

#include <fstream>
#include <memory>

using namespace retdec::gui;
using retdec::gui::test::HeadlessUi;

namespace {
const char* kJson = R"({"sectionTable": {"sections": [
  {"name": ".text", "address": "0x401000", "offset": "0x10", "sizeInFile": "0x20"},
  {"name": ".bss", "address": "0x402000", "offset": "0x0", "sizeInFile": "0x0"},
  {"name": ".big", "address": "0x500000", "offset": "0x0", "sizeInFile": "0x20000"} ]}})";
}

class BinaryBrowserTest : public ::testing::Test {
protected:
	void SetUp() override
	{
		dir = retdec::gui::test::scratchDir("binary_browser");
		std::string bytes;
		for (int i = 0; i < 0x30; ++i)
			bytes += char('A' + (i % 26));
		std::ofstream(dir / "sample.bin", std::ios::binary) << bytes;
		panel = std::make_unique<ui::BinaryBrowserPanel>(h.ui, h.ui.root());
		panel->onAddressActivated = [this](uint64_t a) { activated.push_back(a); };
		panel->onDecompileRangeRequested = [this](const std::string& r) { ranges.push_back(r); };
		h.frame();
	}
	HeadlessUi h;
	std::filesystem::path dir;
	std::unique_ptr<ui::BinaryBrowserPanel> panel;
	std::vector<uint64_t> activated;
	std::vector<std::string> ranges;
};

TEST_F(BinaryBrowserTest, IgnoresFileinfoBeforeLoad)
{
	EXPECT_EQ(panel->panelTitle(), "Binary Browser");
	panel->populateFromFileinfo(kJson);
	EXPECT_EQ(panel->rowCount(), 0);
}

TEST_F(BinaryBrowserTest, LoadShowsHintThenSections)
{
	panel->loadBinary((dir / "sample.bin").string());
	ASSERT_EQ(panel->rowCount(), 1);
	EXPECT_NE(panel->cellText(0, 0).find("Run Inspect"), std::string::npos);
	panel->populateFromFileinfo(kJson);
	ASSERT_EQ(panel->rowCount(), 4);
	EXPECT_EQ(panel->cellText(0, 0), "sample.bin");
	EXPECT_EQ(panel->cellText(1, 0), "    .text");
	EXPECT_EQ(panel->cellText(1, 1), "0x401000");
	EXPECT_EQ(panel->cellText(1, 2), "0x20");
	panel->populateFromFileinfo(R"({"sectionTable": {"sections": []}})");
	ASSERT_EQ(panel->rowCount(), 1);
	EXPECT_EQ(panel->cellText(0, 0), "(no sections in fileinfo)");
}

TEST_F(BinaryBrowserTest, SelectingASectionShowsItsHexDump)
{
	panel->loadBinary((dir / "sample.bin").string());
	panel->populateFromFileinfo(kJson);
	h.frame();
	h.ui.setFocus(panel->table());
	h.key(fuse::ui::Key::Down); // row 0: the file
	EXPECT_TRUE(panel->hexText().empty());
	h.key(fuse::ui::Key::Down); // row 1: .text
	EXPECT_EQ(panel->hexHeaderText(), "Hex View \xE2\x80\x94 .text @ file 0x10 (32 bytes)");
	EXPECT_EQ(panel->hexView().line(0).substr(0, 10), "00000010  ");
	EXPECT_NE(panel->hexText().find("QRSTUVWXYZABCDEF"), std::string::npos);
	EXPECT_EQ(panel->hexView().lineCount(), 3); // two dump lines + trailing newline
	panel->selectRow(2); // .bss, size 0: nothing
	EXPECT_TRUE(panel->hexText().empty());
	panel->selectRow(3); // larger than the 64 KiB cap (and than the file)
	EXPECT_NE(panel->hexText().find("showing first 64 KiB of 128 KiB"), std::string::npos);
}

TEST_F(BinaryBrowserTest, UnreadableBinaryShowsError)
{
	panel->loadBinary((dir / "missing.bin").string());
	panel->populateFromFileinfo(kJson);
	panel->selectRow(1);
	EXPECT_NE(panel->hexText().find("Cannot open binary"), std::string::npos);
}

TEST_F(BinaryBrowserTest, ActivationAndContextMenu)
{
	panel->loadBinary((dir / "sample.bin").string());
	panel->populateFromFileinfo(kJson);
	h.frame();
	h.ui.setFocus(panel->table());
	h.key(fuse::ui::Key::Down);
	h.key(fuse::ui::Key::Down);
	h.key(fuse::ui::Key::Enter);
	ASSERT_EQ(activated.size(), 1u);
	EXPECT_EQ(activated[0], 0x401000u);
	panel->activateRow(0); // the file row has no address
	EXPECT_EQ(activated.size(), 1u);

	panel->openContextMenu({100.f, 100.f}, 1);
	ASSERT_TRUE(panel->contextMenu().isOpen());
	EXPECT_EQ(panel->contextMenu().labels(),
	          (std::vector<std::string>{"Decompile this range\xE2\x80\xA6", "Copy start address", "Copy VMA range", "Copy hex snippet"}));
	EXPECT_TRUE(panel->contextMenu().trigger(0));
	ASSERT_EQ(ranges.size(), 1u);
	EXPECT_EQ(ranges[0], "0x401000-0x401020");
	panel->openContextMenu({100.f, 100.f}, 1);
	panel->contextMenu().trigger(1);
	EXPECT_EQ(h.clipboard(), "0x401000");
	panel->openContextMenu({100.f, 100.f}, 1);
	panel->contextMenu().trigger(2);
	EXPECT_EQ(h.clipboard(), "0x401000-0x401020");
	panel->openContextMenu({100.f, 100.f}, 1);
	panel->contextMenu().trigger(3);
	EXPECT_EQ(h.clipboard().substr(0, 8), "51 52 53"); // bytes at offset 0x10: 'Q' 'R' 'S'
	// No menu for rows without a range.
	h.frame();
	panel->openContextMenu({100.f, 100.f}, 2);
	EXPECT_FALSE(panel->contextMenu().isOpen());
}

TEST_F(BinaryBrowserTest, RightClickOpensMenuForRowUnderPointer)
{
	panel->loadBinary((dir / "sample.bin").string());
	panel->populateFromFileinfo(kJson);
	h.frame();
	const fuse::ui::Rect r = h.ui.contentRect(panel->table());
	bool opened = false;
	for (float y = r.y + 30.f; y < r.bottom() && !opened; y += 6.f)
	{
		h.clickAt({r.x + 50.f, y}, fuse::ui::MouseButton::Right);
		h.frame();
		if (int(h.ui.value(panel->table())) == 1) opened = panel->contextMenu().isOpen();
		else
		{
			panel->contextMenu().close();
			h.frame();
		}
	}
	EXPECT_TRUE(opened);
}

TEST_F(BinaryBrowserTest, ClearResets)
{
	panel->loadBinary((dir / "sample.bin").string());
	panel->populateFromFileinfo(kJson);
	panel->clear();
	EXPECT_EQ(panel->rowCount(), 0);
	EXPECT_TRUE(panel->binaryPath().empty());
	EXPECT_EQ(panel->hexHeaderText(), "Hex View");
}
