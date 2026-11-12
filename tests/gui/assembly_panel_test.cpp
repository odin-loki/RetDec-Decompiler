/**
 * @file tests/gui/assembly_panel_test.cpp
 * @brief AssemblyPanel in a headless UiContext: go-to / find bar, follow jump
 *        (Enter) and back (Escape), the Copy Address / Go to Function menu.
 * @copyright (c) 2026 Odin Loch trading as Imortek
 */

#include "code_ui_test_util.h"

#include "retdec/gui/ui/assembly_panel.h"

#include <gtest/gtest.h>

#include <memory>

using namespace retdec::gui;
using retdec::gui::test::HeadlessUi;
using fuse::ui::Key;

namespace {
const char* kDsm = "; function: main at 0x401000 -- 0x401020\n"
                   "0x401000:   55                        push ebp\n"
                   "0x401001:   89 e5                     mov ebp, esp\n"
                   "0x401003:   e8 f8 00 00 00            call 0x401100 <helper>\n"
                   "0x401008:   75 06                     jne 0x401010\n"
                   "0x40100a:   c3                        ret\n"
                   "0x401010:   31 c0                     xor eax, eax\n"
                   "\n"
                   "; function: helper at 0x401100 -- 0x401110\n"
                   "0x401100:   c3                        ret\n";
}

class AssemblyPanelTest : public ::testing::Test {
protected:
	void SetUp() override
	{
		panel = std::make_unique<ui::AssemblyPanel>(h.ui, h.ui.root());
		panel->onAddressActivated = [this](uint64_t a) { activated.push_back(a); };
		h.frame();
	}
	float lineY(int line)
	{
		const fuse::ui::Rect r = h.ui.contentRect(panel->view().node());
		const float lh = std::ceil(h.ui.computed(panel->view().node()).fontSize * 1.35f);
		return r.y + lh * (float(line - panel->view().firstVisibleLine()) + 0.5f);
	}
	float lineX() { return h.ui.contentRect(panel->view().node()).x + 120.f; }
	HeadlessUi h;
	std::unique_ptr<ui::AssemblyPanel> panel;
	std::vector<uint64_t> activated;
};

TEST_F(AssemblyPanelTest, EmptyStateThenText)
{
	EXPECT_EQ(panel->panelTitle(), "Assembly");
	EXPECT_TRUE(panel->emptyStateShown());
	EXPECT_EQ(panel->emptyState().title(), "No disassembly yet");
	panel->setAssemblyText(kDsm);
	EXPECT_FALSE(panel->emptyStateShown());
	EXPECT_EQ(model::tokenAt(panel->view().tokens(1), 0), model::TokenKind::Address);
	EXPECT_EQ(model::tokenAt(panel->view().tokens(1), std::string_view(kDsm).find("push") - std::string_view(kDsm).find("0x401000:")),
	          model::TokenKind::Mnemonic);
	EXPECT_EQ(model::tokenAt(panel->view().tokens(0), 0), model::TokenKind::Comment);
	panel->clear();
	EXPECT_TRUE(panel->emptyStateShown());
	EXPECT_EQ(panel->functionLabel(), "No function selected");
}

TEST_F(AssemblyPanelTest, NavigateToScrollsAndPendingAddressAppliesLater)
{
	panel->navigateTo(0x401100);
	EXPECT_EQ(panel->functionLabel(), "Address: 0x401100");
	panel->setAssemblyText(kDsm); // address requested before text arrived
	EXPECT_EQ(panel->caretLine(), 9); // the listing line of 0x401100, not the call mentioning it
	panel->onAddressNavigated(0x401010);
	EXPECT_EQ(panel->caretLine(), 6);
	panel->onAddressNavigated(0x401020); // only mentioned in a comment
	EXPECT_EQ(panel->caretLine(), 0);
}

TEST_F(AssemblyPanelTest, GoToAddressBar)
{
	panel->setAssemblyText(kDsm);
	h.frame();
	h.ui.setFocus(panel->view().node());
	h.key(Key::G);
	EXPECT_TRUE(panel->searchBarVisible());
	EXPECT_EQ(panel->searchMode(), ui::AssemblyPanel::SearchMode::GoTo);
	EXPECT_EQ(h.ui.focused(), panel->searchEdit());
	h.type("40100a");
	// Typing 'G' / 'F' into the bar must not re-trigger the shortcuts.
	h.ui.keyDown(Key::F);
	EXPECT_EQ(panel->searchMode(), ui::AssemblyPanel::SearchMode::GoTo);
	h.key(Key::Enter);
	EXPECT_EQ(panel->caretLine(), 5);
	EXPECT_EQ(panel->functionLabel(), "Address: 0x40100a");
	panel->setSearchText("not hex");
	panel->submitSearch(); // ignored
	EXPECT_EQ(panel->caretLine(), 5);
}

TEST_F(AssemblyPanelTest, FindBar)
{
	panel->setAssemblyText(kDsm);
	h.frame();
	h.click(panel->findButton());
	EXPECT_EQ(panel->searchMode(), ui::AssemblyPanel::SearchMode::Find);
	h.type("ret");
	h.key(Key::Enter);
	EXPECT_EQ(panel->caretLine(), 5);
	h.key(Key::Enter);
	EXPECT_EQ(panel->caretLine(), 9);
	h.key(Key::Enter);
	EXPECT_EQ(panel->caretLine(), 5); // wraps
}

TEST_F(AssemblyPanelTest, EnterFollowsJumpAndEscapeGoesBack)
{
	panel->setAssemblyText(kDsm);
	h.frame();
	h.clickAt({lineX(), lineY(3)}); // call 0x401100
	EXPECT_EQ(panel->caretLine(), 3);
	h.key(Key::Enter);
	EXPECT_EQ(panel->caretLine(), 9);
	EXPECT_EQ(panel->backStackSize(), 1u);
	EXPECT_EQ(panel->functionLabel(), "Address: 0x401100");
	h.key(Key::Escape);
	EXPECT_EQ(panel->caretLine(), 3);
	EXPECT_EQ(panel->backStackSize(), 0u);
	// jne follows too; a non-branch line does nothing.
	h.clickAt({lineX(), lineY(4)});
	h.key(Key::Enter);
	EXPECT_EQ(panel->caretLine(), 6);
	h.clickAt({lineX(), lineY(2)});
	h.key(Key::Enter);
	EXPECT_EQ(panel->caretLine(), 2);
}

TEST_F(AssemblyPanelTest, EscapeWithEmptyStackHidesSearchBar)
{
	panel->setAssemblyText(kDsm);
	panel->find();
	EXPECT_TRUE(panel->searchBarVisible());
	h.key(Key::Escape);
	EXPECT_FALSE(panel->searchBarVisible());
}

TEST_F(AssemblyPanelTest, UsesAddressContextMenu)
{
	panel->setAssemblyText(kDsm);
	h.frame();
	h.clickAt({lineX(), lineY(1)}, fuse::ui::MouseButton::Right);
	ASSERT_TRUE(panel->contextMenu().isOpen());
	EXPECT_EQ(panel->contextMenu().labels(), (std::vector<std::string>{"Copy Address", "Go to Function"}));
	EXPECT_TRUE(panel->contextMenu().isEnabled(0));
	EXPECT_TRUE(panel->contextMenu().trigger(0));
	h.frame();
	EXPECT_EQ(h.clipboard(), "0x401000");
	h.clickAt({lineX(), lineY(3)}, fuse::ui::MouseButton::Right);
	EXPECT_TRUE(panel->contextMenu().trigger(1));
	h.frame();
	ASSERT_EQ(activated.size(), 1u);
	EXPECT_EQ(activated[0], 0x401003u);
	// Blank line: both disabled.
	h.clickAt({lineX(), lineY(7)}, fuse::ui::MouseButton::Right);
	ASSERT_TRUE(panel->contextMenu().isOpen());
	EXPECT_FALSE(panel->contextMenu().isEnabled(0));
	EXPECT_FALSE(panel->contextMenu().isEnabled(1));
	EXPECT_FALSE(panel->contextMenu().trigger(1));
	panel->contextMenu().close();
	h.frame();
	EXPECT_EQ(activated.size(), 1u);
}

TEST_F(AssemblyPanelTest, MenuPickedWithThePointer)
{
	panel->setAssemblyText(kDsm);
	h.frame();
	const fuse::ui::Vec2 at{lineX(), lineY(4)};
	h.clickAt(at, fuse::ui::MouseButton::Right);
	h.frame();
	// The second item of the popup sits below the first; click it.
	const fuse::ui::NodeHandle overlay = fuse::ui::widgets::overlayLayer(h.ui);
	fuse::ui::NodeHandle scope = h.ui.firstChild(overlay);
	while (h.ui.alive(h.ui.nextSibling(scope)))
		scope = h.ui.nextSibling(scope);
	const fuse::ui::NodeHandle menu = h.ui.firstChild(scope);
	const fuse::ui::NodeHandle second = h.ui.nextSibling(h.ui.firstChild(menu));
	ASSERT_TRUE(h.ui.alive(second));
	EXPECT_EQ(h.ui.text(second), "Go to Function");
	h.click(second);
	ASSERT_EQ(activated.size(), 1u);
	EXPECT_EQ(activated[0], 0x401008u);
	EXPECT_FALSE(panel->contextMenu().isOpen());
}
