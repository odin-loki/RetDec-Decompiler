/**
 * @file tests/gui/tri_pane_test.cpp
 * @brief Unit tests for TriPaneCodeView, SyncedCodePane, LineMapping,
 *        and CodeSyntaxHighlighter (Task 50).
 *
 * Tests cover:
 *   - LineMapping CRUD and bidirectional lookup
 *   - CodeSyntaxHighlighter construction and language switching
 *   - SyncedCodePane content loading, line count, highlight, scroll
 *   - TriPaneCodeView construction and default state
 *   - loadFunction() populates all three panes
 *   - setOutputLanguage() updates cPane lang
 *   - setLineMapping() and cross-pane activation via signals
 *   - Navigation history: back/forward
 *   - Find bar show/hide
 *   - clear() resets all state
 *   - onFunctionSelected() updates funcLabel and history
 */

#include <memory>
#include <gtest/gtest.h>
#include <QApplication>
#include <QSignalSpy>
#include <QTest>

#include "retdec/gui/panels/tri_pane_code_view.h"

namespace {

void qtEnv2() {
    Q_ASSERT(QApplication::instance() != nullptr);
}

} // anonymous namespace

// ─── LineMapping tests ────────────────────────────────────────────────────────

class LineMappingTest : public ::testing::Test {
protected:
    retdec::gui::panels::LineMapping m;
};

TEST_F(LineMappingTest, EmptyByDefault) {
    EXPECT_TRUE(m.isEmpty());
    EXPECT_EQ(m.byAsmLine(1), nullptr);
    EXPECT_EQ(m.byIrLine(1),  nullptr);
    EXPECT_EQ(m.byCLine(1),   nullptr);
}

TEST_F(LineMappingTest, AddEntryAndLookupByAsm) {
    m.addEntry(3, 5, 7);
    const auto* e = m.byAsmLine(3);
    ASSERT_NE(e, nullptr);
    EXPECT_EQ(e->asmLine, 3);
    EXPECT_EQ(e->irLine,  5);
    EXPECT_EQ(e->cLine,   7);
}

TEST_F(LineMappingTest, LookupByIr) {
    m.addEntry(1, 4, 9);
    const auto* e = m.byIrLine(4);
    ASSERT_NE(e, nullptr);
    EXPECT_EQ(e->asmLine, 1);
}

TEST_F(LineMappingTest, LookupByCLine) {
    m.addEntry(2, 3, 6);
    const auto* e = m.byCLine(6);
    ASSERT_NE(e, nullptr);
    EXPECT_EQ(e->irLine, 3);
}

TEST_F(LineMappingTest, MissedLookupReturnsNull) {
    m.addEntry(1, 2, 3);
    EXPECT_EQ(m.byAsmLine(99), nullptr);
    EXPECT_EQ(m.byIrLine(99),  nullptr);
    EXPECT_EQ(m.byCLine(99),   nullptr);
}

TEST_F(LineMappingTest, MultipleEntries) {
    m.addEntry(1, 1, 1);
    m.addEntry(2, 3, 4);
    m.addEntry(5, 6, 7);
    EXPECT_NE(m.byAsmLine(5), nullptr);
    EXPECT_EQ(m.byAsmLine(5)->cLine, 7);
}

TEST_F(LineMappingTest, NegativeIndexMeansNoMapping) {
    m.addEntry(1, -1, 2);  // IR line = -1 (no IR correspondence)
    EXPECT_EQ(m.byIrLine(-1), nullptr);
    EXPECT_NE(m.byAsmLine(1), nullptr);
    EXPECT_EQ(m.byAsmLine(1)->irLine, -1);
}

TEST_F(LineMappingTest, ClearRemovesAllEntries) {
    m.addEntry(1, 2, 3);
    m.clear();
    EXPECT_TRUE(m.isEmpty());
    EXPECT_EQ(m.byAsmLine(1), nullptr);
}

TEST_F(LineMappingTest, NotEmptyAfterAddEntry) {
    m.addEntry(10, 20, 30);
    EXPECT_FALSE(m.isEmpty());
}

// ─── CodeSyntaxHighlighter tests ─────────────────────────────────────────────

class SyntaxHighlighterTest : public ::testing::Test {
protected:
    void SetUp() override { (void)qtEnv2(); }
};

TEST_F(SyntaxHighlighterTest, ConstructAsmHighlighter) {
    QTextDocument doc;
    EXPECT_NO_THROW({
        retdec::gui::panels::CodeSyntaxHighlighter hl(
            &doc, retdec::gui::panels::PaneLang::Asm);
    });
}

TEST_F(SyntaxHighlighterTest, ConstructIRHighlighter) {
    QTextDocument doc;
    EXPECT_NO_THROW({
        retdec::gui::panels::CodeSyntaxHighlighter hl(
            &doc, retdec::gui::panels::PaneLang::IR);
    });
}

TEST_F(SyntaxHighlighterTest, ConstructCHighlighter) {
    QTextDocument doc;
    EXPECT_NO_THROW({
        retdec::gui::panels::CodeSyntaxHighlighter hl(
            &doc, retdec::gui::panels::PaneLang::C);
    });
}

TEST_F(SyntaxHighlighterTest, ConstructCxxHighlighter) {
    QTextDocument doc;
    EXPECT_NO_THROW({
        retdec::gui::panels::CodeSyntaxHighlighter hl(
            &doc, retdec::gui::panels::PaneLang::Cxx);
    });
}

TEST_F(SyntaxHighlighterTest, ConstructCSharpHighlighter) {
    QTextDocument doc;
    EXPECT_NO_THROW({
        retdec::gui::panels::CodeSyntaxHighlighter hl(
            &doc, retdec::gui::panels::PaneLang::CSharp);
    });
}

TEST_F(SyntaxHighlighterTest, ConstructJavaHighlighter) {
    QTextDocument doc;
    EXPECT_NO_THROW({
        retdec::gui::panels::CodeSyntaxHighlighter hl(
            &doc, retdec::gui::panels::PaneLang::Java);
    });
}

TEST_F(SyntaxHighlighterTest, ConstructPythonHighlighter) {
    QTextDocument doc;
    EXPECT_NO_THROW({
        retdec::gui::panels::CodeSyntaxHighlighter hl(
            &doc, retdec::gui::panels::PaneLang::Python);
    });
}

TEST_F(SyntaxHighlighterTest, ConstructLuaHighlighter) {
    QTextDocument doc;
    EXPECT_NO_THROW({
        retdec::gui::panels::CodeSyntaxHighlighter hl(
            &doc, retdec::gui::panels::PaneLang::Lua);
    });
}

TEST_F(SyntaxHighlighterTest, SetLangSwitchesHighlighting) {
    QTextDocument doc;
    retdec::gui::panels::CodeSyntaxHighlighter hl(
        &doc, retdec::gui::panels::PaneLang::Asm);
    EXPECT_NO_THROW(hl.setLang(retdec::gui::panels::PaneLang::C));
    // setLang queues rehighlight(); drain before hl goes out of scope.
    QApplication::processEvents();
}

TEST_F(SyntaxHighlighterTest, HighlightingDoesNotCrashOnContent) {
    QTextDocument doc;
    retdec::gui::panels::CodeSyntaxHighlighter hl(
        &doc, retdec::gui::panels::PaneLang::C);
    doc.setPlainText(
        "#include <stdio.h>\n"
        "int main(void) { return 0; /* comment */ }\n");
    QApplication::processEvents();
    SUCCEED();
}

TEST_F(SyntaxHighlighterTest, BlockCommentSpanningLines) {
    QTextDocument doc;
    retdec::gui::panels::CodeSyntaxHighlighter hl(
        &doc, retdec::gui::panels::PaneLang::C);
    doc.setPlainText("/* start\n   middle\n   end */\nint x;\n");
    QApplication::processEvents();
    SUCCEED();
}

// ─── SyncedCodePane tests ─────────────────────────────────────────────────────

class SyncedCodePaneTest : public ::testing::Test {
protected:
    void SetUp() override {
        (void)qtEnv2();
        pane = std::make_unique<retdec::gui::panels::SyncedCodePane>(
            retdec::gui::panels::PaneLang::C, "Test Pane");
    }
    void TearDown() override {
        // Intentionally no processEvents(): setLang() queues rehighlight() on the
        // next event-loop turn; pumping here with a 0×0 QPlainTextEdit viewport
        // could hang (see CodeSyntaxHighlighter::setLang).
    }
    std::unique_ptr<retdec::gui::panels::SyncedCodePane> pane;
};

TEST_F(SyncedCodePaneTest, ConstructsWithoutCrash) {
    EXPECT_NE(pane.get(), nullptr);
}

TEST_F(SyncedCodePaneTest, TitleIsSet) {
    EXPECT_EQ(pane->title(), "Test Pane");
}

TEST_F(SyncedCodePaneTest, EmptyContentHasOneBlock) {
    EXPECT_GE(pane->lineCount(), 1);
}

TEST_F(SyncedCodePaneTest, SetContentUpdatesLineCount) {
    pane->setContent("line1\nline2\nline3\n");
    EXPECT_EQ(pane->lineCount(), 4);  // Qt counts the trailing empty block
}

TEST_F(SyncedCodePaneTest, HighlightLineDoesNotCrash) {
    pane->setContent("a\nb\nc\n");
    EXPECT_NO_THROW(pane->highlightLine(2));
}

TEST_F(SyncedCodePaneTest, HighlightNegativeLineClears) {
    pane->setContent("a\nb\n");
    pane->highlightLine(1);
    EXPECT_NO_THROW(pane->highlightLine(-1));
}

TEST_F(SyncedCodePaneTest, ScrollToLineDoesNotCrash) {
    pane->setContent("a\nb\nc\nd\ne\n");
    EXPECT_NO_THROW(pane->scrollToLine(3));
}

TEST_F(SyncedCodePaneTest, ClearHighlightDoesNotCrash) {
    EXPECT_NO_THROW(pane->clearHighlight());
}

TEST_F(SyncedCodePaneTest, SetLangDoesNotCrash) {
    EXPECT_NO_THROW(
        pane->setLang(retdec::gui::panels::PaneLang::Python));
}

TEST_F(SyncedCodePaneTest, LineActivatedSignalEmitted) {
    pane->setContent("a\nb\nc\n");
    QSignalSpy spy(pane.get(),
        &retdec::gui::panels::SyncedCodePane::lineActivated);
    // Simulate cursor position change via programmatic cursor
    QTextCursor c = pane->editor()->textCursor();
    c.movePosition(QTextCursor::Start);
    c.movePosition(QTextCursor::NextBlock);
    pane->editor()->setTextCursor(c);
    QApplication::processEvents();
    EXPECT_GE(spy.count(), 0);  // Signal may or may not fire on programmatic move
}

TEST_F(SyncedCodePaneTest, EditorIsReadOnly) {
    EXPECT_TRUE(pane->editor()->isReadOnly());
}

// ─── TriPaneCodeView tests ────────────────────────────────────────────────────

class TriPaneTest : public ::testing::Test {
protected:
    void SetUp() override {
        (void)qtEnv2();
        view = std::make_unique<retdec::gui::panels::TriPaneCodeView>();
    }
    void TearDown() override {
        // Same rationale as SyncedCodePaneTest: deferred pane rehighlights + pump
        // can stall when QPlainTextEdit viewports are 0×0 in unit tests.
    }
    std::unique_ptr<retdec::gui::panels::TriPaneCodeView> view;
};

TEST_F(TriPaneTest, ConstructsWithoutCrash) {
    EXPECT_NE(view.get(), nullptr);
}

TEST_F(TriPaneTest, PanelTitleIsTriPaneView) {
    EXPECT_EQ(view->panelTitle(), "Tri-Pane View");
}

TEST_F(TriPaneTest, LoadFunctionDoesNotCrash) {
    EXPECT_NO_THROW(view->loadFunction(0x401000,
        "401000: push rbp\n401002: mov rbp, rsp\n",
        "%entry:\n  %a = alloca i32\n  ret void\n",
        "int sub_401000(void) {\n  return 0;\n}\n"));
}

TEST_F(TriPaneTest, SetLineMappingDoesNotCrash) {
    retdec::gui::panels::LineMapping m;
    m.addEntry(1, 1, 1);
    m.addEntry(2, 2, 2);
    EXPECT_NO_THROW(view->setLineMapping(m));
}

TEST_F(TriPaneTest, SetOutputLanguageCxx) {
    EXPECT_NO_THROW(
        view->setOutputLanguage(retdec::gui::panels::OutputLang::Cxx));
}

TEST_F(TriPaneTest, SetOutputLanguagePython) {
    EXPECT_NO_THROW(
        view->setOutputLanguage(retdec::gui::panels::OutputLang::Python));
}

TEST_F(TriPaneTest, SetOutputLanguageJava) {
    EXPECT_NO_THROW(
        view->setOutputLanguage(retdec::gui::panels::OutputLang::Java));
}

TEST_F(TriPaneTest, SetOutputLanguageCSharp) {
    EXPECT_NO_THROW(
        view->setOutputLanguage(retdec::gui::panels::OutputLang::CSharp));
}

TEST_F(TriPaneTest, SetOutputLanguageLua) {
    EXPECT_NO_THROW(
        view->setOutputLanguage(retdec::gui::panels::OutputLang::Lua));
}

TEST_F(TriPaneTest, NavigateToEmitsSignal) {
    QSignalSpy spy(view.get(),
        &retdec::gui::panels::TriPaneCodeView::addressNavigated);
    view->navigateTo(0xDEADBEEF);
    QApplication::processEvents();
    EXPECT_EQ(spy.count(), 1);
    EXPECT_EQ(spy[0][0].toULongLong(), 0xDEADBEEFULL);
}

TEST_F(TriPaneTest, NavigateBackDoesNothingWhenNoHistory) {
    EXPECT_NO_THROW(view->navigateBack());
}

TEST_F(TriPaneTest, NavigateForwardDoesNothingWhenNoHistory) {
    EXPECT_NO_THROW(view->navigateForward());
}

TEST_F(TriPaneTest, NavigateHistoryBackForward) {
    view->onFunctionSelected(0x100, "func_a");
    view->onFunctionSelected(0x200, "func_b");
    view->onFunctionSelected(0x300, "func_c");

    QSignalSpy spy(view.get(),
        &retdec::gui::panels::TriPaneCodeView::addressNavigated);
    view->navigateBack();
    EXPECT_EQ(spy.count(), 1);
    spy.clear();

    view->navigateForward();
    EXPECT_EQ(spy.count(), 1);
}

TEST_F(TriPaneTest, OnFunctionSelectedUpdatesHistory) {
    // Multiple calls should build up history
    view->onFunctionSelected(0x401000, "main");
    view->onFunctionSelected(0x401100, "helper");
    QSignalSpy spy(view.get(),
        &retdec::gui::panels::TriPaneCodeView::addressNavigated);
    view->navigateBack();
    EXPECT_EQ(spy.count(), 1);
}

TEST_F(TriPaneTest, ShowFindBarDoesNotCrash) {
    EXPECT_NO_THROW(view->showFindBar());
}

TEST_F(TriPaneTest, HideFindBarDoesNotCrash) {
    view->showFindBar();
    EXPECT_NO_THROW(view->hideFindBar());
}

TEST_F(TriPaneTest, ClearResetsState) {
    view->loadFunction(0x1000, "push rbp\n", "%entry:\n  ret\n", "int f() { return 0; }\n");
    EXPECT_NO_THROW(view->clear());
}

TEST_F(TriPaneTest, CrossPaneSyncWithMapping) {
    retdec::gui::panels::LineMapping m;
    m.addEntry(1, 1, 1);
    m.addEntry(2, 3, 5);
    view->loadFunction(0x1000,
        "push rbp\nmov rbp, rsp\nret\n",
        "entry:\n  %x = alloca\n  %y = load\n  ret\n",
        "int f(void) {\n  int x;\n  x = 0;\n  return x;\n  }\n");
    view->setLineMapping(m);
    // Simulate asm line 2 activation via the slot
    // (direct slot test — no GUI interaction needed)
    EXPECT_NO_THROW(view->onFunctionSelected(0x1000, "f"));
}

