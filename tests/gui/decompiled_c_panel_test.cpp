/**
 * @file tests/gui/decompiled_c_panel_test.cpp
 * @brief DecompiledCPanel in a headless UiContext (includes the Qt
 *        polish_integration / comprehensive_smoke checks for this panel).
 * @copyright (c) 2026 Odin Loch trading as Imortek
 */

#include "code_ui_test_util.h"

#include "retdec/gui/ui/decompiled_c_panel.h"

#include <gtest/gtest.h>

#include <fstream>
#include <memory>

using namespace retdec::gui;
using fuse::ui::Key;
using fuse::ui::kModCtrl;
using fuse::ui::kModShift;
using retdec::gui::test::HeadlessUi;

namespace {
const char* kSource =
	"#include <stdio.h>\n"
	"\n"
	"// Address range: 0x401000 - 0x401030\n"
	"int helper(int a) {\n"
	"    return a + 1; // 0x401004\n"
	"}\n"
	"\n"
	"int main(void) {\n"
	"    int x = helper(2);\n"
	"    printf(\"%d\\n\", x);\n"
	"    return function_401200(x);\n"
	"}\n";
}

class DecompiledCPanelTest : public ::testing::Test {
protected:
	void SetUp() override
	{
		panel = std::make_unique<ui::DecompiledCPanel>(h.ui, h.ui.root());
		panel->onStatusMessage = [this](const std::string& m, int) { status = m; };
		h.frame();
	}
	HeadlessUi h;
	std::unique_ptr<ui::DecompiledCPanel> panel;
	std::string status;
};

TEST_F(DecompiledCPanelTest, StartsWithEmptyState)
{
	EXPECT_EQ(panel->panelTitle(), "Decompiled C");
	EXPECT_TRUE(panel->emptyStateShown());
	EXPECT_EQ(panel->emptyState().title(), "No decompiled output yet");
	EXPECT_NE(panel->emptyState().hint().find("F5"), std::string::npos);
	EXPECT_EQ(panel->functionLabel(), "No function selected");
	EXPECT_FALSE(panel->findBarVisible());
	EXPECT_TRUE(h.ui.disabled(panel->refinedToggle()));
}

TEST_F(DecompiledCPanelTest, SetSourceShowsHighlightedText)
{
	panel->setSource(kSource);
	h.frame();
	EXPECT_FALSE(panel->emptyStateShown());
	EXPECT_EQ(panel->documentText(), kSource);
	EXPECT_EQ(panel->view().line(3), "int helper(int a) {");
	EXPECT_EQ(model::tokenAt(panel->view().tokens(3), 0), model::TokenKind::Keyword);
	EXPECT_EQ(model::tokenAt(panel->view().tokens(0), 0), model::TokenKind::Preprocessor);
	EXPECT_EQ(model::tokenAt(panel->view().tokens(4), 20), model::TokenKind::Comment);
	panel->setSource("");
	EXPECT_TRUE(panel->emptyStateShown());
}

TEST_F(DecompiledCPanelTest, AcceptsEmptyAndLargeSources)
{
	panel->setSource(std::string{});
	panel->setSource(std::string(200'000, 'a'));
	EXPECT_EQ(panel->documentText().size(), 200'000u);
	std::string big;
	while (big.size() < 3 * 1024 * 1024)
		big += "int v = 0; // padding padding padding\n";
	panel->setSource(big);
	h.frame();
	EXPECT_EQ(panel->documentText().size(), big.size());
	EXPECT_NE(panel->functionLabel().find("Loaded"), std::string::npos);
}

TEST_F(DecompiledCPanelTest, FunctionLabelAndScrollToFunction)
{
	panel->setSource(kSource);
	h.frame();
	panel->onFunctionSelected(0x401000, "helper");
	EXPECT_EQ(panel->functionLabel(), "helper @ 0x401000");
	panel->scrollToFunction("main", 8);
	EXPECT_EQ(panel->view().caret().line, 7);
	panel->scrollToFunction("helper", -1); // by name: `\bhelper\s*\(` definition
	EXPECT_EQ(panel->view().caret().line, 3);
	panel->scrollToFunction(0x401200, ""); // function_401200
	EXPECT_EQ(panel->view().caret().line, 10);
	panel->scrollToFunction(0x401000, "nope"); // 0x401000 comment
	EXPECT_EQ(panel->view().caret().line, 2);
}

TEST_F(DecompiledCPanelTest, FindBarShortcutsAndSearch)
{
	panel->setSource(kSource);
	h.frame();
	h.ui.setFocus(panel->view().node());
	h.key(Key::F, kModCtrl);
	EXPECT_TRUE(panel->findBarVisible());
	EXPECT_EQ(h.ui.focused(), panel->findEdit());
	h.type("helper");
	h.key(Key::Enter);
	EXPECT_EQ(panel->lastFindQuery(), "helper");
	EXPECT_EQ(panel->view().selectedText(), "helper");
	EXPECT_EQ(panel->view().caret().line, 3);
	h.key(Key::F3);
	EXPECT_EQ(panel->view().caret().line, 8);
	h.key(Key::F3);
	EXPECT_EQ(panel->view().caret().line, 3); // wraps
	h.key(Key::F3, kModShift);
	EXPECT_EQ(panel->view().caret().line, 8);
	h.key(Key::Escape);
	EXPECT_FALSE(panel->findBarVisible());
	// F3 with the bar hidden reuses the last query.
	h.ui.setFocus(panel->view().node());
	h.key(Key::F3);
	EXPECT_EQ(panel->view().caret().line, 3);
}

TEST_F(DecompiledCPanelTest, FindNextWithoutQueryOpensBar)
{
	panel->setSource(kSource);
	panel->findNext();
	EXPECT_TRUE(panel->findBarVisible());
}

TEST_F(DecompiledCPanelTest, SearchingEmptyOutputReportsStatus)
{
	panel->showFindBar();
	panel->setFindText("x");
	panel->submitFind();
	EXPECT_EQ(status, "No decompiled output to search");
}

TEST_F(DecompiledCPanelTest, CopyAndSave)
{
	panel->setSource(kSource);
	h.frame();
	h.click(panel->copyButton());
	EXPECT_EQ(h.clipboard(), kSource);
	EXPECT_EQ(status, "Copied to clipboard");
	const auto dir = retdec::gui::test::scratchDir("decompiled_c");
	ASSERT_TRUE(panel->saveTo(dir / "out.c"));
	EXPECT_EQ(status, "Saved: " + (dir / "out.c").string());
	std::ifstream in(dir / "out.c");
	std::string content((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
	EXPECT_EQ(content, kSource);
	EXPECT_FALSE(panel->saveTo(dir / "missing" / "x.c"));
	h.click(panel->saveButton());
	EXPECT_TRUE(h.ui.alive(panel->saveDialog()));
}

TEST_F(DecompiledCPanelTest, RefinedToggle)
{
	panel->setSource("int original;\n");
	panel->setRefinedSource("int refined;\n");
	EXPECT_FALSE(h.ui.disabled(panel->refinedToggle()));
	h.frame();
	h.click(panel->refinedToggle());
	EXPECT_TRUE(panel->showingRefined());
	EXPECT_EQ(panel->documentText(), "int refined;\n");
	h.click(panel->refinedToggle());
	EXPECT_EQ(panel->documentText(), "int original;\n");
	panel->setRefinedSource("");
	EXPECT_TRUE(h.ui.disabled(panel->refinedToggle()));
}

TEST_F(DecompiledCPanelTest, ContextMenuGoToFunctionAndCopy)
{
	panel->setSource(kSource);
	h.frame();
	std::vector<uint64_t> activated;
	panel->onAddressActivated = [&](uint64_t a) { activated.push_back(a); };
	const fuse::ui::Rect r = h.ui.contentRect(panel->view().node());
	const float lh = std::ceil(h.ui.computed(panel->view().node()).fontSize * 1.35f);
	h.clickAt({r.x + 80.f, r.y + lh * 4.5f}, fuse::ui::MouseButton::Right); // line 5: "// 0x401004"
	ASSERT_TRUE(panel->contextMenu().isOpen());
	const int go = panel->contextMenu().indexOf(ui::kGoToFunctionLabel);
	ASSERT_GE(go, 0);
	EXPECT_TRUE(panel->contextMenu().isEnabled(std::size_t(go)));
	EXPECT_TRUE(panel->contextMenu().trigger(std::size_t(go)));
	h.frame();
	ASSERT_EQ(activated.size(), 1u);
	EXPECT_EQ(activated[0], 0x401004u);
	// Copy Address on the same line.
	h.clickAt({r.x + 80.f, r.y + lh * 4.5f}, fuse::ui::MouseButton::Right);
	EXPECT_TRUE(panel->contextMenu().trigger(std::size_t(panel->contextMenu().indexOf(ui::kCopyAddressLabel))));
	h.frame();
	EXPECT_EQ(h.clipboard(), "0x401004");
	// A line without an address disables both.
	h.clickAt({r.x + 80.f, r.y + lh * 0.5f}, fuse::ui::MouseButton::Right);
	EXPECT_FALSE(panel->contextMenu().isEnabled(std::size_t(panel->contextMenu().indexOf(ui::kCopyAddressLabel))));
	panel->contextMenu().close();
}

TEST_F(DecompiledCPanelTest, CtrlAAndCtrlCCopySelection)
{
	panel->setSource("abc\ndef");
	h.frame();
	h.ui.setFocus(panel->view().node());
	h.key(Key::A, kModCtrl);
	h.key(Key::C, kModCtrl);
	EXPECT_EQ(h.clipboard(), "abc\ndef");
}

TEST_F(DecompiledCPanelTest, ClearResets)
{
	panel->setSource(kSource);
	panel->onFunctionSelected(1, "f");
	panel->showFindBar();
	panel->clear();
	EXPECT_TRUE(panel->emptyStateShown());
	EXPECT_EQ(panel->functionLabel(), "No function selected");
	EXPECT_FALSE(panel->findBarVisible());
	EXPECT_TRUE(panel->lastFindQuery().empty());
}

TEST_F(DecompiledCPanelTest, OtherOutputLanguageHighlighting)
{
	panel->setLanguage(model::CodeLang::Python);
	panel->setSource("def f():\n    return None\n");
	EXPECT_EQ(model::tokenAt(panel->view().tokens(0), 0), model::TokenKind::Keyword);
}
