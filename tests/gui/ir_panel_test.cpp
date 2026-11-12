/**
 * @file tests/gui/ir_panel_test.cpp
 * @brief IRPanel in a headless UiContext: Function / Module stage switch,
 *        IR highlighting, find bar.
 * @copyright (c) 2026 Odin Loch trading as Imortek
 */

#include "code_ui_test_util.h"

#include "retdec/gui/ui/ir_panel.h"

#include <gtest/gtest.h>

#include <memory>

using namespace retdec::gui;
using retdec::gui::test::HeadlessUi;
using fuse::ui::Key;

class IRPanelTest : public ::testing::Test {
protected:
	void SetUp() override
	{
		panel = std::make_unique<ui::IRPanel>(h.ui, h.ui.root());
		panel->onStatusMessage = [this](const std::string& s, int) { status = s; };
		h.frame();
	}
	HeadlessUi h;
	std::unique_ptr<ui::IRPanel> panel;
	std::string status;
};

TEST_F(IRPanelTest, EmptyState)
{
	EXPECT_EQ(panel->panelTitle(), "IR (SSA)");
	EXPECT_TRUE(panel->emptyStateShown());
	EXPECT_EQ(panel->emptyState().title(), "No IR loaded");
	EXPECT_EQ(fuse::ui::widgets::comboText(h.ui, panel->stageCombo()), "Function");
}

TEST_F(IRPanelTest, SetIRTextStoresPerStageAndHighlights)
{
	panel->setIRText("define i32 @f() {\nentry:\n  ret i32 0\n}\n"); // default stage "SSA" -> function IR
	EXPECT_FALSE(panel->emptyStateShown());
	EXPECT_EQ(panel->stage(), ui::IRPanel::Stage::Function);
	EXPECT_EQ(model::tokenAt(panel->view().tokens(0), 0), model::TokenKind::Keyword);
	EXPECT_EQ(model::tokenAt(panel->view().tokens(1), 0), model::TokenKind::Label);
	panel->setIRText("; ModuleID = 'a'\n@g = global i32 0\n", "Module");
	EXPECT_EQ(panel->stage(), ui::IRPanel::Stage::Module);
	EXPECT_EQ(fuse::ui::widgets::comboText(h.ui, panel->stageCombo()), "Module");
	EXPECT_EQ(panel->view().line(1), "@g = global i32 0");
	EXPECT_EQ(panel->moduleIr(), "; ModuleID = 'a'\n@g = global i32 0\n");
	EXPECT_EQ(panel->functionIr(), "define i32 @f() {\nentry:\n  ret i32 0\n}\n");
}

TEST_F(IRPanelTest, StageComboSwitchesBetweenFunctionAndModule)
{
	panel->setIRText("FUNCTION", "Function");
	panel->setIRText("MODULE", "Module");
	// The user picks "Function" in the combo.
	h.ui.setValue(panel->stageCombo(), 0.f, true);
	EXPECT_EQ(panel->view().text(), "FUNCTION");
	EXPECT_EQ(status, "IR stage: Function");
	h.ui.setFocus(panel->stageCombo());
	h.key(Key::Down);
	EXPECT_EQ(panel->view().text(), "MODULE");
	EXPECT_EQ(status, "IR stage: Module");
	panel->setStage(ui::IRPanel::Stage::Function);
	EXPECT_EQ(panel->view().text(), "FUNCTION");
}

TEST_F(IRPanelTest, EmptyStageKeepsCurrentText)
{
	panel->setIRText("FUNCTION", "Function");
	panel->setStage(ui::IRPanel::Stage::Module); // no module IR yet
	EXPECT_EQ(panel->view().text(), "FUNCTION");
}

TEST_F(IRPanelTest, FunctionLabelAndClear)
{
	panel->onFunctionSelected(0x401000, "main");
	EXPECT_EQ(panel->functionLabel(), "main @ 0x401000");
	panel->setIRText("x");
	panel->clear();
	EXPECT_TRUE(panel->emptyStateShown());
	EXPECT_TRUE(panel->functionIr().empty());
	EXPECT_EQ(panel->functionLabel(), "No function selected");
}

TEST_F(IRPanelTest, FindBar)
{
	panel->setIRText("%1 = add i32 %a, 1\n%2 = add i32 %1, %a\nret i32 %2\n");
	h.frame();
	h.ui.setFocus(panel->view().node());
	h.key(Key::F, fuse::ui::kModCtrl);
	EXPECT_TRUE(panel->findBarVisible());
	h.type("%a");
	h.key(Key::Enter);
	EXPECT_EQ(panel->view().caret(), (model::TextPos{0, 15}));
	h.key(Key::F3);
	EXPECT_EQ(panel->view().caret().line, 1);
	h.key(Key::F3, fuse::ui::kModShift);
	EXPECT_EQ(panel->view().caret().line, 0);
	h.key(Key::Escape);
	EXPECT_FALSE(panel->findBarVisible());
}

TEST_F(IRPanelTest, ContextMenuOnAddressComment)
{
	panel->setIRText("  ret i32 0 ; 0x401010\n");
	h.frame();
	std::vector<uint64_t> got;
	panel->onAddressActivated = [&](uint64_t a) { got.push_back(a); };
	const fuse::ui::Rect r = h.ui.contentRect(panel->view().node());
	h.clickAt({r.x + 60.f, r.y + 5.f}, fuse::ui::MouseButton::Right);
	ASSERT_TRUE(panel->contextMenu().isOpen());
	EXPECT_TRUE(panel->contextMenu().trigger(1));
	ASSERT_EQ(got.size(), 1u);
	EXPECT_EQ(got[0], 0x401010u);
}
