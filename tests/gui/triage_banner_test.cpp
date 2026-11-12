/**
 * @file tests/gui/triage_banner_test.cpp
 * @brief TriageBanner in a headless UiContext (includes the Qt
 *        comprehensive-smoke "banner shows after open" check).
 * @copyright (c) 2026 Odin Loch trading as Imortek
 */

#include "code_ui_test_util.h"

#include "retdec/gui/ui/triage_banner.h"

#include <gtest/gtest.h>

#include <fstream>
#include <memory>

using namespace retdec::gui;
using retdec::gui::test::HeadlessUi;

class TriageBannerTest : public ::testing::Test {
protected:
	void SetUp() override
	{
		banner = std::make_unique<ui::TriageBanner>(h.ui, h.ui.root());
		banner->onUnpackRequested = [this] { events.push_back("unpack"); };
		banner->onDecompileRequested = [this] { events.push_back("decompile"); };
		banner->onMoreActionsRequested = [this](fuse::ui::Vec2) { events.push_back("more"); };
		banner->onDismissed = [this] { events.push_back("dismissed"); };
		banner->onTargetDetailsRequested = [this] { events.push_back("target"); };
		h.frame();
	}
	HeadlessUi h;
	std::unique_ptr<ui::TriageBanner> banner;
	std::vector<std::string> events;
};

TEST_F(TriageBannerTest, HiddenUntilABinaryIsSet)
{
	EXPECT_TRUE(banner->isHidden());
	const auto dir = retdec::gui::test::scratchDir("triage");
	std::ofstream(dir / "fake.bin") << std::string(256, 'x');
	banner->setBinary((dir / "fake.bin").string());
	h.frame();
	EXPECT_FALSE(banner->isHidden());
	EXPECT_TRUE(banner->actionsEnabled());
	EXPECT_EQ(banner->titleText(), "fake.bin");
	EXPECT_EQ(banner->titleTooltip(), (dir / "fake.bin").string());
	EXPECT_EQ(banner->sizeText(), "256 B");
	EXPECT_EQ(banner->formatText(), "\xE2\x80\xA6");
	EXPECT_EQ(banner->archText(), "\xE2\x80\xA6");
	EXPECT_FALSE(banner->packerShown());
	EXPECT_GT(h.ui.rect(banner->root()).h, 0.f);
	banner->setBinary("");
	EXPECT_TRUE(banner->isHidden());
	EXPECT_EQ(banner->formatText(), "\xE2\x80\x94");
}

TEST_F(TriageBannerTest, MetadataAndPackerBadge)
{
	banner->setBinary("/nonexistent/thing.exe");
	banner->setMetadata("PE", "", "Windows", 3 * 1024 * 1024, "UPX");
	EXPECT_EQ(banner->formatText(), "PE");
	EXPECT_EQ(banner->archText(), "\xE2\x80\x94");
	EXPECT_EQ(banner->osText(), "Windows");
	EXPECT_EQ(banner->sizeText(), "3.0 MiB");
	EXPECT_TRUE(banner->packerShown());
	EXPECT_EQ(banner->packerText(), "UPX-packed");
	banner->setMetadata("PE", "x86", "Windows", 0);
	EXPECT_FALSE(banner->packerShown());
	EXPECT_EQ(banner->sizeText(), "3.0 MiB"); // kept
	model::FileinfoInfo info;
	info.format = "ELF";
	info.arch = "ARM";
	info.os = "Linux";
	banner->setFileinfo(info, 2048);
	EXPECT_EQ(banner->archText(), "ARM");
	EXPECT_EQ(banner->sizeText(), "2.0 KiB");
}

TEST_F(TriageBannerTest, ButtonsBadgesAndDismiss)
{
	banner->setBinary("/x/a.out");
	h.frame();
	h.click(banner->unpackButton());
	h.click(banner->decompileButton());
	h.click(banner->moreButton());
	h.click(banner->archBadge());
	h.click(banner->osBadge());
	EXPECT_EQ(events, (std::vector<std::string>{"unpack", "decompile", "more", "target", "target"}));
	banner->setActionsEnabled(false);
	EXPECT_FALSE(banner->actionsEnabled());
	h.click(banner->decompileButton());
	EXPECT_EQ(events.size(), 5u);
	banner->setActionsEnabled(true);
	h.click(banner->closeButton());
	EXPECT_EQ(events.back(), "dismissed");
	EXPECT_TRUE(banner->isHidden());
}
