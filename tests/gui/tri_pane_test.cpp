/**
 * @file tests/gui/tri_pane_test.cpp
 * @brief TriPaneCodeView, SyncedCodePane, LineMapping and the code tokenizer
 *        (port of the Qt tri_pane_test.cpp, plus behaviour the Qt tests only
 *        smoke-tested: cross-pane highlight, scroll sync, find, shortcuts).
 * @copyright (c) 2026 Odin Loch trading as Imortek
 */

#include "code_ui_test_util.h"

#include "retdec/gui/model/code_highlight.h"
#include "retdec/gui/model/tri_pane_sync.h"
#include "retdec/gui/ui/tri_pane_code_view.h"

#include <gtest/gtest.h>

#include <memory>
#include <vector>

using namespace retdec::gui;
using retdec::gui::test::HeadlessUi;
using fuse::ui::Key;

// ─── LineMapping ─────────────────────────────────────────────────────────────

class LineMappingTest : public ::testing::Test {
protected:
	model::LineMapping m;
};

TEST_F(LineMappingTest, EmptyByDefault)
{
	EXPECT_TRUE(m.isEmpty());
	EXPECT_EQ(m.byAsmLine(1), nullptr);
	EXPECT_EQ(m.byIrLine(1), nullptr);
	EXPECT_EQ(m.byCLine(1), nullptr);
}

TEST_F(LineMappingTest, AddEntryAndLookupByAsm)
{
	m.addEntry(3, 5, 7);
	const auto* e = m.byAsmLine(3);
	ASSERT_NE(e, nullptr);
	EXPECT_EQ(e->asmLine, 3);
	EXPECT_EQ(e->irLine, 5);
	EXPECT_EQ(e->cLine, 7);
}

TEST_F(LineMappingTest, LookupByIr)
{
	m.addEntry(1, 4, 9);
	const auto* e = m.byIrLine(4);
	ASSERT_NE(e, nullptr);
	EXPECT_EQ(e->asmLine, 1);
}

TEST_F(LineMappingTest, LookupByCLine)
{
	m.addEntry(2, 3, 6);
	const auto* e = m.byCLine(6);
	ASSERT_NE(e, nullptr);
	EXPECT_EQ(e->irLine, 3);
}

TEST_F(LineMappingTest, MissedLookupReturnsNull)
{
	m.addEntry(1, 2, 3);
	EXPECT_EQ(m.byAsmLine(99), nullptr);
	EXPECT_EQ(m.byIrLine(99), nullptr);
	EXPECT_EQ(m.byCLine(99), nullptr);
}

TEST_F(LineMappingTest, MultipleEntries)
{
	m.addEntry(1, 1, 1);
	m.addEntry(2, 3, 4);
	m.addEntry(5, 6, 7);
	EXPECT_NE(m.byAsmLine(5), nullptr);
	EXPECT_EQ(m.byAsmLine(5)->cLine, 7);
}

TEST_F(LineMappingTest, NegativeIndexMeansNoMapping)
{
	m.addEntry(1, -1, 2);
	EXPECT_EQ(m.byIrLine(-1), nullptr);
	EXPECT_NE(m.byAsmLine(1), nullptr);
	EXPECT_EQ(m.byAsmLine(1)->irLine, -1);
}

TEST_F(LineMappingTest, ClearRemovesAllEntries)
{
	m.addEntry(1, 2, 3);
	m.clear();
	EXPECT_TRUE(m.isEmpty());
	EXPECT_EQ(m.byAsmLine(1), nullptr);
}

TEST_F(LineMappingTest, NotEmptyAfterAddEntry)
{
	m.addEntry(10, 20, 30);
	EXPECT_FALSE(m.isEmpty());
}

TEST_F(LineMappingTest, ByLineDispatchesOnPane)
{
	m.addEntry(1, 2, 3);
	EXPECT_EQ(m.byLine(model::TriPane::IR, 2), m.byAsmLine(1));
	EXPECT_EQ(m.byLine(model::TriPane::C, 3), m.byAsmLine(1));
}

// ─── Tokenizer (the Qt CodeSyntaxHighlighter) ────────────────────────────────

class SyntaxHighlighterTest : public ::testing::Test {};

TEST_F(SyntaxHighlighterTest, EveryLanguageTokenizesWithoutCrash)
{
	const std::vector<std::string> lines = {"#include <stdio.h>", "int main(void) { return 0; /* comment */ }", "  push rbp",
	                                        "%a = alloca i32", "def f(): return None  # c", "local x = 1 -- c", ""};
	for (model::CodeLang l: {model::CodeLang::Asm, model::CodeLang::IR, model::CodeLang::C, model::CodeLang::Cxx,
	                         model::CodeLang::CSharp, model::CodeLang::Java, model::CodeLang::Python, model::CodeLang::Lua})
		EXPECT_NO_THROW({ (void)model::highlightLines(l, lines); });
}

TEST_F(SyntaxHighlighterTest, SetLangSwitchesHighlighting)
{
	int st = model::kStateNormal;
	const auto asmSpans = model::highlightLine(model::CodeLang::Asm, "push rbp", st);
	st = model::kStateNormal;
	const auto cSpans = model::highlightLine(model::CodeLang::C, "push rbp", st);
	EXPECT_EQ(model::tokenAt(asmSpans, 0), model::TokenKind::Mnemonic);
	EXPECT_EQ(model::tokenAt(cSpans, 0), model::TokenKind::Text);
}

TEST_F(SyntaxHighlighterTest, HighlightingDoesNotCrashOnContent)
{
	const auto spans = model::highlightLines(model::CodeLang::C, {"#include <stdio.h>", "int main(void) { return 0; /* comment */ }"});
	ASSERT_EQ(spans.size(), 2u);
	EXPECT_EQ(model::tokenAt(spans[0], 0), model::TokenKind::Preprocessor);
	EXPECT_EQ(model::tokenAt(spans[1], 0), model::TokenKind::Keyword); // int
}

TEST_F(SyntaxHighlighterTest, BlockCommentSpanningLines)
{
	const auto spans = model::highlightLines(model::CodeLang::C, {"/* start", "   middle", "   end */", "int x;"});
	EXPECT_EQ(model::tokenAt(spans[0], 0), model::TokenKind::Comment);
	EXPECT_EQ(model::tokenAt(spans[1], 4), model::TokenKind::Comment);
	EXPECT_EQ(model::tokenAt(spans[2], 3), model::TokenKind::Comment);
	EXPECT_EQ(model::tokenAt(spans[3], 0), model::TokenKind::Keyword);
}

TEST_F(SyntaxHighlighterTest, BlockStateDoesNotLeakIntoPython)
{
	// Switching from C to Python must not keep a stale block-comment state.
	int st = model::kStateInBlockComment;
	const auto spans = model::highlightLine(model::CodeLang::Python, "def f():", st);
	EXPECT_EQ(st, model::kStateNormal);
	EXPECT_EQ(model::tokenAt(spans, 0), model::TokenKind::Keyword);
}

// ─── SyncedCodePane ──────────────────────────────────────────────────────────

class SyncedCodePaneTest : public ::testing::Test {
protected:
	void SetUp() override
	{
		pane = std::make_unique<ui::SyncedCodePane>(h.ui, h.ui.root(), model::CodeLang::C, "Test Pane");
		h.frame();
	}
	HeadlessUi h;
	std::unique_ptr<ui::SyncedCodePane> pane;
};

TEST_F(SyncedCodePaneTest, ConstructsWithoutCrash)
{
	EXPECT_NE(pane.get(), nullptr);
	EXPECT_TRUE(h.ui.alive(pane->root()));
}

TEST_F(SyncedCodePaneTest, TitleIsSet)
{
	EXPECT_EQ(pane->title(), "Test Pane");
}

TEST_F(SyncedCodePaneTest, EmptyContentHasOneBlock)
{
	EXPECT_GE(pane->lineCount(), 1);
}

TEST_F(SyncedCodePaneTest, SetContentUpdatesLineCount)
{
	pane->setContent("line1\nline2\nline3\n");
	EXPECT_EQ(pane->lineCount(), 4); // trailing empty block, as QTextDocument
}

TEST_F(SyncedCodePaneTest, HighlightLineDoesNotCrash)
{
	pane->setContent("a\nb\nc\n");
	EXPECT_NO_THROW(pane->highlightLine(2));
	EXPECT_EQ(pane->highlightedLine(), 2);
}

TEST_F(SyncedCodePaneTest, HighlightNegativeLineClears)
{
	pane->setContent("a\nb\n");
	pane->highlightLine(1);
	EXPECT_NO_THROW(pane->highlightLine(-1));
	EXPECT_EQ(pane->highlightedLine(), -1);
}

TEST_F(SyncedCodePaneTest, ScrollToLineDoesNotCrash)
{
	pane->setContent("a\nb\nc\nd\ne\n");
	EXPECT_NO_THROW(pane->scrollToLine(3));
	EXPECT_EQ(pane->editor().caret().line, 2);
}

TEST_F(SyncedCodePaneTest, ClearHighlightDoesNotCrash)
{
	EXPECT_NO_THROW(pane->clearHighlight());
}

TEST_F(SyncedCodePaneTest, SetLangDoesNotCrash)
{
	EXPECT_NO_THROW(pane->setLang(model::CodeLang::Python));
	EXPECT_EQ(pane->lang(), model::CodeLang::Python);
}

TEST_F(SyncedCodePaneTest, LineActivatedSignalEmitted)
{
	pane->setContent("a\nb\nc\n");
	h.frame();
	std::vector<int> activated;
	pane->onLineActivated = [&](int l) { activated.push_back(l); };
	// Click on the second line of the editor.
	const fuse::ui::Rect r = h.ui.contentRect(pane->editor().node());
	const float lh = std::ceil(h.ui.computed(pane->editor().node()).fontSize * 1.35f);
	h.clickAt({r.x + 60.f, r.y + lh * 1.5f});
	ASSERT_FALSE(activated.empty());
	EXPECT_EQ(activated.back(), 2);
	// Keyboard moves activate too.
	h.key(Key::Down);
	EXPECT_EQ(activated.back(), 3);
}

TEST_F(SyncedCodePaneTest, EditorIsReadOnly)
{
	EXPECT_TRUE(pane->isReadOnly());
	pane->setContent("abc");
	h.ui.setFocus(pane->editor().node());
	h.type("xyz");
	h.key(Key::Backspace);
	EXPECT_EQ(pane->editor().text(), "abc");
}

// ─── TriPaneCodeView ─────────────────────────────────────────────────────────

class TriPaneTest : public ::testing::Test {
protected:
	void SetUp() override
	{
		view = std::make_unique<ui::TriPaneCodeView>(h.ui, h.ui.root());
		view->onAddressNavigated = [this](uint64_t a) { navigated.push_back(a); };
		h.frame();
	}
	HeadlessUi h;
	std::unique_ptr<ui::TriPaneCodeView> view;
	std::vector<uint64_t> navigated;
};

TEST_F(TriPaneTest, ConstructsWithoutCrash)
{
	EXPECT_NE(view.get(), nullptr);
	EXPECT_EQ(view->functionLabel(), "No function loaded");
}

TEST_F(TriPaneTest, PanelTitleIsTriPaneView)
{
	EXPECT_EQ(view->panelTitle(), "Tri-Pane View");
}

TEST_F(TriPaneTest, LoadFunctionDoesNotCrash)
{
	EXPECT_NO_THROW(view->loadFunction(0x401000, "401000: push rbp\n401002: mov rbp, rsp\n",
	                                   "%entry:\n  %a = alloca i32\n  ret void\n", "int sub_401000(void) {\n  return 0;\n}\n"));
	EXPECT_EQ(view->asmPane().lineCount(), 3);
	EXPECT_EQ(view->irPane().lineCount(), 4);
	EXPECT_EQ(view->cPane().editor().line(0), "int sub_401000(void) {");
	EXPECT_EQ(view->functionLabel(), "0x401000");
}

TEST_F(TriPaneTest, SetLineMappingDoesNotCrash)
{
	model::LineMapping m;
	m.addEntry(1, 1, 1);
	m.addEntry(2, 2, 2);
	EXPECT_NO_THROW(view->setLineMapping(m));
	EXPECT_EQ(view->lineMapping().size(), 2u);
}

TEST_F(TriPaneTest, SetOutputLanguages)
{
	view->setOutputLanguage(ui::OutputLang::Cxx);
	EXPECT_EQ(view->cPane().lang(), model::CodeLang::Cxx);
	EXPECT_EQ(view->cPane().title(), "Decompiled C++");
	view->setOutputLanguage(ui::OutputLang::Python);
	EXPECT_EQ(view->cPane().title(), "Decompiled Python");
	view->setOutputLanguage(ui::OutputLang::Java);
	EXPECT_EQ(view->cPane().title(), "Decompiled Java");
	view->setOutputLanguage(ui::OutputLang::CSharp);
	EXPECT_EQ(view->cPane().title(), "Decompiled C#");
	view->setOutputLanguage(ui::OutputLang::Lua);
	EXPECT_EQ(view->cPane().title(), "Decompiled Lua");
	EXPECT_EQ(int(h.ui.value(view->langCombo())), int(ui::OutputLang::Lua));
}

TEST_F(TriPaneTest, LanguageFromSettings)
{
	view->syncLanguageFromSettings(" C++ ");
	EXPECT_EQ(view->outputLanguage(), ui::OutputLang::Cxx);
	view->syncLanguageFromSettings("py");
	EXPECT_EQ(view->outputLanguage(), ui::OutputLang::Python);
	view->syncLanguageFromSettings("unknown");
	EXPECT_EQ(view->outputLanguage(), ui::OutputLang::C);
}

TEST_F(TriPaneTest, LangComboChangesHighlightingAndHints)
{
	std::string status;
	view->onStatusMessage = [&](const std::string& s, int) { status = s; };
	h.ui.setValue(view->langCombo(), 3.f, true); // Java
	EXPECT_EQ(view->outputLanguage(), ui::OutputLang::Java);
	EXPECT_NE(status.find("Output language"), std::string::npos);
}

TEST_F(TriPaneTest, NavigateToEmitsSignal)
{
	view->navigateTo(0xDEADBEEF);
	ASSERT_EQ(navigated.size(), 1u);
	EXPECT_EQ(navigated[0], 0xDEADBEEFULL);
}

TEST_F(TriPaneTest, NavigateBackDoesNothingWhenNoHistory)
{
	EXPECT_NO_THROW(view->navigateBack());
	EXPECT_TRUE(navigated.empty());
}

TEST_F(TriPaneTest, NavigateForwardDoesNothingWhenNoHistory)
{
	EXPECT_NO_THROW(view->navigateForward());
	EXPECT_TRUE(navigated.empty());
}

TEST_F(TriPaneTest, NavigateHistoryBackForward)
{
	view->onFunctionSelected(0x100, "func_a");
	view->onFunctionSelected(0x200, "func_b");
	view->onFunctionSelected(0x300, "func_c");
	EXPECT_TRUE(h.ui.disabled(view->forwardButton()));
	EXPECT_FALSE(h.ui.disabled(view->backButton()));

	view->navigateBack();
	ASSERT_EQ(navigated.size(), 1u);
	EXPECT_EQ(navigated[0], 0x200u);
	navigated.clear();

	view->navigateForward();
	ASSERT_EQ(navigated.size(), 1u);
	EXPECT_EQ(navigated[0], 0x300u);
}

TEST_F(TriPaneTest, OnFunctionSelectedUpdatesHistory)
{
	view->onFunctionSelected(0x401000, "main");
	view->onFunctionSelected(0x401100, "helper");
	EXPECT_EQ(view->functionLabel(), "helper  @ 0x401100");
	view->navigateBack();
	ASSERT_EQ(navigated.size(), 1u);
	EXPECT_EQ(navigated[0], 0x401000u);
}

TEST_F(TriPaneTest, BackForwardButtonsAndAltArrows)
{
	view->onFunctionSelected(0x10, "a");
	view->onFunctionSelected(0x20, "b");
	h.frame();
	h.click(view->backButton());
	ASSERT_EQ(navigated.size(), 1u);
	EXPECT_EQ(navigated.back(), 0x10u);
	h.ui.setFocus(view->asmPane().editor().node());
	h.key(Key::Right, fuse::ui::kModAlt);
	EXPECT_EQ(navigated.back(), 0x20u);
	h.key(Key::Left, fuse::ui::kModAlt);
	EXPECT_EQ(navigated.back(), 0x10u);
}

TEST_F(TriPaneTest, HistoryIsCappedAt32)
{
	for (int i = 0; i < 40; ++i)
		view->onFunctionSelected(0x1000 + uint64_t(i), "f");
	int steps = 0;
	while (view->canGoBack())
	{
		view->navigateBack();
		++steps;
	}
	EXPECT_EQ(steps, 31);
	EXPECT_EQ(navigated.back(), 0x1000u + 8u);
}

TEST_F(TriPaneTest, ShowFindBarDoesNotCrash)
{
	EXPECT_NO_THROW(view->showFindBar());
	EXPECT_TRUE(view->findBarVisible());
	EXPECT_EQ(h.ui.focused(), view->findEdit());
}

TEST_F(TriPaneTest, HideFindBarDoesNotCrash)
{
	view->showFindBar();
	EXPECT_NO_THROW(view->hideFindBar());
	EXPECT_FALSE(view->findBarVisible());
}

TEST_F(TriPaneTest, CtrlFOpensAndEscapeClosesFindBar)
{
	view->loadFunction(0x1000, "push rbp\n", "ret\n", "int f(void);\n");
	h.ui.setFocus(view->cPane().editor().node());
	h.key(Key::F, fuse::ui::kModCtrl);
	EXPECT_TRUE(view->findBarVisible());
	h.key(Key::Escape);
	EXPECT_FALSE(view->findBarVisible());
}

TEST_F(TriPaneTest, FindSearchesAllPanes)
{
	view->loadFunction(0x1000, "push rbp\nmov eax, 1\n", "%x = add i32 1, 2\n", "int x = 1;\nreturn x;\n");
	view->showFindBar();
	h.type("x");
	h.key(Key::Enter);
	EXPECT_EQ(view->asmPane().editor().selectedText(), "x"); // "eax"
	EXPECT_EQ(view->irPane().editor().selectedText(), "x");
	EXPECT_EQ(view->cPane().editor().selectedText(), "x");
	EXPECT_EQ(view->cPane().editor().caret().line, 0);
	view->findNext();
	EXPECT_EQ(view->cPane().editor().caret().line, 1);
	view->findPrevious();
	EXPECT_EQ(view->cPane().editor().caret().line, 0);
}

TEST_F(TriPaneTest, ClearResetsState)
{
	view->loadFunction(0x1000, "push rbp\n", "%entry:\n  ret\n", "int f() { return 0; }\n");
	view->onFunctionSelected(0x1000, "f");
	EXPECT_NO_THROW(view->clear());
	EXPECT_TRUE(view->asmPane().editor().isEmpty());
	EXPECT_TRUE(view->lineMapping().isEmpty());
	EXPECT_FALSE(view->canGoBack());
	EXPECT_EQ(view->functionLabel(), "No function loaded");
}

TEST_F(TriPaneTest, CrossPaneSyncWithMapping)
{
	model::LineMapping m;
	m.addEntry(1, 1, 1);
	m.addEntry(2, 3, 5);
	view->loadFunction(0x1000, "push rbp\nmov rbp, rsp\nret\n", "entry:\n  %x = alloca\n  %y = load\n  ret\n",
	                   "int f(void) {\n  int x;\n  x = 0;\n  return x;\n  }\n");
	view->setLineMapping(m);
	EXPECT_NO_THROW(view->onFunctionSelected(0x1000, "f"));
	view->activateLine(model::TriPane::Asm, 2);
	EXPECT_EQ(view->asmPane().highlightedLine(), 2);
	EXPECT_EQ(view->irPane().highlightedLine(), 3);
	EXPECT_EQ(view->cPane().highlightedLine(), 5);
	EXPECT_EQ(view->cPane().editor().caret().line, 4);
	// From the C side back.
	view->activateLine(model::TriPane::C, 1);
	EXPECT_EQ(view->asmPane().highlightedLine(), 1);
	EXPECT_EQ(view->irPane().highlightedLine(), 1);
}

TEST_F(TriPaneTest, ClickingALineSyncsTheOtherPanes)
{
	model::LineMapping m;
	m.addEntry(2, 3, 4);
	view->loadFunction(0x1000, "a\nb\nc\n", "1\n2\n3\n4\n", "w\nx\ny\nz\n");
	view->setLineMapping(m);
	h.frame();
	const fuse::ui::NodeHandle ed = view->asmPane().editor().node();
	const fuse::ui::Rect r = h.ui.contentRect(ed);
	const float lh = std::ceil(h.ui.computed(ed).fontSize * 1.35f);
	h.clickAt({r.x + 40.f, r.y + lh * 1.5f});
	EXPECT_EQ(view->irPane().highlightedLine(), 3);
	EXPECT_EQ(view->cPane().highlightedLine(), 4);
}

TEST_F(TriPaneTest, MappingFromAddresses)
{
	view->loadFunction(0x401000, "0x401000:  push ebp\n0x401001:  mov ebp, esp\n0x401003:  ret\n",
	                   "define i32 @f() {\n  %1 = alloca i32 ; 0x401001\n  ret i32 0 ; 0x401003\n}\n",
	                   "int f(void) { // 0x401000\n  int a; // 0x401001\n  return 0; // 0x401003\n}\n");
	view->buildLineMappingFromAddresses();
	view->activateLine(model::TriPane::Asm, 2);
	EXPECT_EQ(view->irPane().highlightedLine(), 2);
	EXPECT_EQ(view->cPane().highlightedLine(), 2);
	view->activateLine(model::TriPane::C, 3);
	EXPECT_EQ(view->asmPane().highlightedLine(), 3);
	EXPECT_EQ(view->irPane().highlightedLine(), 3);
}

TEST_F(TriPaneTest, ScrollingOnePaneScrollsTheOthersProportionally)
{
	std::string a, b, c;
	for (int i = 0; i < 400; ++i)
		a += "line\n";
	for (int i = 0; i < 200; ++i)
		b += "line\n";
	for (int i = 0; i < 800; ++i)
		c += "line\n";
	view->loadFunction(0x1, a, b, c);
	h.frame();
	ui::CodeView& av = view->asmPane().editor();
	h.ui.wheel(h.center(av.node()), {0.f, -20.f}); // 60 lines down
	h.frame();
	EXPECT_EQ(av.firstVisibleLine(), 60);
	const int expectIr = model::proportionalScroll(60, av.scrollMax(), view->irPane().editor().scrollMax());
	const int expectC = model::proportionalScroll(60, av.scrollMax(), view->cPane().editor().scrollMax());
	EXPECT_EQ(view->irPane().editor().firstVisibleLine(), expectIr);
	EXPECT_EQ(view->cPane().editor().firstVisibleLine(), expectC);
	EXPECT_GT(expectC, 60);
}

TEST_F(TriPaneTest, RendersHeadless)
{
	view->loadFunction(0x401000, "0x401000:  55  push ebp\n0x401001:  call 0x401100\n", "define i32 @main() {\nentry:\n  ret i32 0\n}\n",
	                   "int main(void) {\n    return 0; // 0x401001\n}\n");
	h.frame(2);
	EXPECT_GT(h.ui.drawList().size(), 20u);
	const char* out = std::getenv("RETDEC_GUI_TEST_PNG_DIR");
	if (out) EXPECT_TRUE(h.writePng(std::string(out) + "/tri_pane.png"));
}
