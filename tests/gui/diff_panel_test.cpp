/**
 * @file tests/gui/diff_panel_test.cpp
 * @brief Diff: the Qt MyersDiff / DiffPanel tests ported to the model and the
 *        FuseUI panel, plus engine, view-row and headless UI tests.
 * @copyright (c) 2026 Odin Loch trading as Imortek
 */

#include "retdec/gui/model/diff_engine.h"
#include "retdec/gui/model/diff_view.h"
#include "retdec/gui/ui/diff_panel.h"

#include "table_panels_test_util.h"

#include <gtest/gtest.h>

#include <filesystem>
#include <format>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

using namespace retdec::gui::model;
using retdec::gui::ui::DiffPanel;
using retdec_gui_tp_test::Headless;
namespace w = fuse::ui::widgets;

// ─── Ported: MyersDiff ───────────────────────────────────────────────────────

TEST(DiffPanelTest, DiffIdenticalTexts)
{
	auto r = MyersDiff::diffText("a\nb\nc\n", "a\nb\nc\n");
	EXPECT_EQ(r.linesAdded, 0);
	EXPECT_EQ(r.linesRemoved, 0);
	EXPECT_GT(r.linesEqual, 0);
	EXPECT_DOUBLE_EQ(r.similarity, 1.0);
}

TEST(DiffPanelTest, DiffCompletelyDifferent)
{
	auto r = MyersDiff::diffText("a\nb\nc\n", "x\ny\nz\n");
	EXPECT_EQ(r.linesAdded, 3);
	EXPECT_EQ(r.linesRemoved, 3);
	EXPECT_EQ(r.linesEqual, 0);
	EXPECT_DOUBLE_EQ(r.similarity, 0.0);
}

TEST(DiffPanelTest, DiffOneLineAdded)
{
	auto r = MyersDiff::diffText("a\nb\n", "a\nb\nc\n");
	EXPECT_EQ(r.linesAdded, 1);
	EXPECT_EQ(r.linesRemoved, 0);
	EXPECT_EQ(r.linesEqual, 2);
}

TEST(DiffPanelTest, DiffOneLineRemoved)
{
	auto r = MyersDiff::diffText("a\nb\nc\n", "a\nc\n");
	EXPECT_EQ(r.linesAdded, 0);
	EXPECT_EQ(r.linesRemoved, 1);
	EXPECT_EQ(r.linesEqual, 2);
}

TEST(DiffPanelTest, DiffLineChanged)
{
	auto r = MyersDiff::diffText("a\nfoo\nc\n", "a\nbar\nc\n");
	EXPECT_EQ(r.linesAdded, 1);
	EXPECT_EQ(r.linesRemoved, 1);
	EXPECT_EQ(r.linesEqual, 2);
}

TEST(DiffPanelTest, DiffEmptyLeft)
{
	auto r = MyersDiff::diffText("", "a\nb\n");
	EXPECT_EQ(r.linesAdded, 2);
	EXPECT_EQ(r.linesRemoved, 0);
}

TEST(DiffPanelTest, DiffEmptyRight)
{
	auto r = MyersDiff::diffText("a\nb\n", "");
	EXPECT_EQ(r.linesAdded, 0);
	EXPECT_EQ(r.linesRemoved, 2);
}

TEST(DiffPanelTest, DiffBothEmpty)
{
	auto r = MyersDiff::diffText("", "");
	EXPECT_EQ(r.linesAdded, 0);
	EXPECT_EQ(r.linesRemoved, 0);
	EXPECT_EQ(r.linesEqual, 0);
	EXPECT_DOUBLE_EQ(r.similarity, 1.0);
}

TEST(DiffPanelTest, DiffSimilarityRange)
{
	auto r = MyersDiff::diffText("a\nb\nc\nd\n", "a\nb\nX\nd\n");
	EXPECT_GE(r.similarity, 0.0);
	EXPECT_LE(r.similarity, 1.0);
}

TEST(DiffPanelTest, DiffOpsCountMatchesLines)
{
	auto r = MyersDiff::diffText("a\nb\nc\n", "a\nX\nc\nd\n");
	EXPECT_EQ(r.linesAdded + r.linesRemoved + r.linesEqual, static_cast<int>(r.ops.size()));
}

TEST(DiffPanelTest, DiffEqualOpsContainCorrectLines)
{
	auto r = MyersDiff::diffText("a\nb\nc\n", "a\nb\nc\n");
	for (const auto& op: r.ops)
	{
		EXPECT_EQ(op.kind, DiffOpKind::Equal);
		EXPECT_FALSE(op.line.empty());
	}
}

TEST(DiffPanelTest, DiffInsertOpsHaveNoLeftLine)
{
	auto r = MyersDiff::diffText("a\n", "a\nb\n");
	bool found = false;
	for (const auto& op: r.ops)
		if (op.kind == DiffOpKind::Insert)
		{
			found = true;
			EXPECT_EQ(op.leftLine, -1);
		}
	EXPECT_TRUE(found);
}

TEST(DiffPanelTest, DiffDeleteOpsHaveNoRightLine)
{
	auto r = MyersDiff::diffText("a\nb\n", "a\n");
	bool found = false;
	for (const auto& op: r.ops)
		if (op.kind == DiffOpKind::Delete)
		{
			found = true;
			EXPECT_EQ(op.rightLine, -1);
		}
	EXPECT_TRUE(found);
}

// ─── Ported: toUnifiedDiff / toHtml ──────────────────────────────────────────

TEST(DiffPanelTest, UnifiedDiffHeader)
{
	auto s = MyersDiff::diffText("a\n", "b\n").toUnifiedDiff("left.c", "right.c");
	EXPECT_NE(s.find("--- left.c"), std::string::npos);
	EXPECT_NE(s.find("+++ right.c"), std::string::npos);
}

TEST(DiffPanelTest, UnifiedDiffHunkHeader)
{
	EXPECT_NE(MyersDiff::diffText("a\nb\nc\n", "a\nX\nc\n").toUnifiedDiff().find("@@"), std::string::npos);
}

TEST(DiffPanelTest, UnifiedDiffContainsMinusPlus)
{
	auto s = MyersDiff::diffText("old\n", "new\n").toUnifiedDiff();
	EXPECT_NE(s.find("-old"), std::string::npos);
	EXPECT_NE(s.find("+new"), std::string::npos);
}

TEST(DiffPanelTest, UnifiedDiffEmptyForIdentical)
{
	EXPECT_EQ(MyersDiff::diffText("same\n", "same\n").toUnifiedDiff().find("@@"), std::string::npos);
}

TEST(DiffPanelTest, HtmlOutputContainsTags)
{
	auto s = MyersDiff::diffText("a\n", "b\n").toHtml();
	EXPECT_NE(s.find("<html>"), std::string::npos);
	EXPECT_NE(s.find("</html>"), std::string::npos);
}

TEST(DiffPanelTest, HtmlEscapesSpecialChars)
{
	auto s = MyersDiff::diffText("<tag>\n", "<tag>\n").toHtml();
	EXPECT_EQ(s.find("<tag>"), std::string::npos);
	EXPECT_NE(s.find("&lt;"), std::string::npos);
}

// ─── Ported: DiffPanel (headless FuseUI) ─────────────────────────────────────

TEST(DiffPanelTest, PanelConstruction)
{
	Headless h;
	DiffPanel panel(h.ui, h.ui.root());
	EXPECT_TRUE(h.ui.alive(panel.root()));
	EXPECT_EQ(panel.statsText(), "No diff");
}

TEST(DiffPanelTest, SetDiffUpdatesCurrentDiff)
{
	Headless h;
	DiffPanel panel(h.ui, h.ui.root());
	panel.setDiff("a\nb\n", "a\nX\n");
	EXPECT_FALSE(panel.currentDiff().isEmpty());
}

TEST(DiffPanelTest, SetDiffEmitsDiffChanged)
{
	Headless h;
	DiffPanel panel(h.ui, h.ui.root());
	bool emitted = false;
	panel.onDiffChanged = [&](const DiffResult&) { emitted = true; };
	panel.setDiff("old\n", "new\n");
	EXPECT_TRUE(emitted);
}

TEST(DiffPanelTest, ClearResetsDiff)
{
	Headless h;
	DiffPanel panel(h.ui, h.ui.root());
	panel.setDiff("x\n", "y\n");
	panel.clear();
	EXPECT_TRUE(panel.currentDiff().isEmpty());
	EXPECT_EQ(h.ui.text(panel.stageCombo()), "<manual>");
}

TEST(DiffPanelTest, AddStageRegistersStage)
{
	Headless h;
	DiffPanel panel(h.ui, h.ui.root());
	panel.addStage("after_typing", "int x;\n", "int x = 0;\n");
	EXPECT_FALSE(panel.currentDiff().isEmpty() && panel.currentDiff().linesAdded == 0);
	// index 0 = <manual>, index 1 = after_typing
	EXPECT_EQ(h.ui.text(panel.stageCombo()), "<manual>\nafter_typing");
	EXPECT_EQ(panel.stages().size(), 1u);
	EXPECT_EQ(h.ui.text(panel.leftLabel()), "Before: after_typing");
}

TEST(DiffPanelTest, IdenticalDiffNoOps)
{
	Headless h;
	DiffPanel panel(h.ui, h.ui.root());
	panel.setDiff("same\nsame2\n", "same\nsame2\n");
	EXPECT_EQ(panel.currentDiff().linesAdded, 0);
	EXPECT_EQ(panel.currentDiff().linesRemoved, 0);
}

TEST(DiffPanelTest, DiffWithManyLines)
{
	std::string left, right;
	for (int i = 0; i < 100; ++i)
		left += "line " + std::to_string(i) + "\n";
	for (int i = 0; i < 100; ++i)
	{
		if (i == 50) right += "inserted\n";
		right += "line " + std::to_string(i) + "\n";
	}
	Headless h;
	DiffPanel panel(h.ui, h.ui.root());
	panel.setDiff(left, right);
	EXPECT_EQ(panel.currentDiff().linesAdded, 1);
	EXPECT_EQ(panel.currentDiff().linesRemoved, 0);
}

TEST(DiffPanelTest, SimilarityIsOneForIdentical)
{
	Headless h;
	DiffPanel panel(h.ui, h.ui.root());
	panel.setDiff("a\nb\nc\n", "a\nb\nc\n");
	EXPECT_DOUBLE_EQ(panel.currentDiff().similarity, 1.0);
}

// ─── Engine: same edit script as the Qt panel ────────────────────────────────

namespace {

std::string script(const DiffResult& r)
{
	std::string s;
	for (const auto& op: r.ops)
	{
		s += op.kind == DiffOpKind::Equal ? ' ' : op.kind == DiffOpKind::Delete ? '-' : '+';
		s += op.line;
		s += std::format("({},{})", op.leftLine, op.rightLine);
		s += ';';
	}
	return s;
}

} // namespace

TEST(DiffEngineTest, ScriptMatchesQtTieBreaking)
{
	// The LCS table is walked from the end and prefers the deletion on ties;
	// the reversed walk therefore lists a replaced line's insertion first.
	EXPECT_EQ(script(MyersDiff::diffText("a\nfoo\nc\n", "a\nbar\nc\n")), " a(0,0);+bar(-1,1);-foo(1,-1); c(2,2);");
	EXPECT_EQ(script(MyersDiff::diffText("x\na\ny\n", "a\nz\n")), "-x(0,-1); a(1,0);+z(-1,1);-y(2,-1);");
	EXPECT_EQ(script(MyersDiff::diffText("a\nb\n", "b\na\n")), "+b(-1,0); a(0,1);-b(1,-1);");
	const auto r = MyersDiff::diffText("a\nb\nc\nd\ne\nf\ng\nh\ni\nj\n", "a\nB\nc\nd\ne\nf\ng\nh\nI\nj\n");
	EXPECT_EQ(
		r.toUnifiedDiff("l", "r", 1),
		"--- l\n+++ r\n@@ -1,3 +1,3 @@\n a\n+B\n-b\n c\n@@ -8,3 +8,3 @@\n h\n+I\n-i\n j\n");
	EXPECT_EQ(
		r.toUnifiedDiff("l", "r", 3),
		"--- l\n+++ r\n@@ -1,10 +1,10 @@\n a\n+B\n-b\n c\n d\n e\n f\n g\n h\n+I\n-i\n j\n");
	EXPECT_TRUE(r.exact);
}

TEST(DiffEngineTest, OversizedMiddleIsWholesaleReplacement)
{
	std::vector<std::string> a, b;
	for (int i = 0; i < 6000; ++i)
	{
		a.push_back("a" + std::to_string(i));
		b.push_back("b" + std::to_string(i));
	}
	a.insert(a.begin(), "head");
	b.insert(b.begin(), "head");
	const auto r = MyersDiff::diff(a, b);
	EXPECT_FALSE(r.exact);
	EXPECT_EQ(r.linesEqual, 1);
	EXPECT_EQ(r.linesRemoved, 6000);
	EXPECT_EQ(r.linesAdded, 6000);
	EXPECT_EQ(r.ops[1].kind, DiffOpKind::Delete);
	EXPECT_EQ(r.ops[1].leftLine, 1);
	EXPECT_EQ(r.ops.back().kind, DiffOpKind::Insert);
	EXPECT_EQ(r.ops.back().rightLine, 6000);
}

TEST(DiffEngineTest, SplitLinesLikeGetline)
{
	EXPECT_EQ(MyersDiff::splitLines("a\nb"), (std::vector<std::string>{"a", "b"}));
	EXPECT_EQ(MyersDiff::splitLines("a\n\nb\n"), (std::vector<std::string>{"a", "", "b"}));
	EXPECT_TRUE(MyersDiff::splitLines("").empty());
}

TEST(DiffEngineTest, IntraLineSpans)
{
	auto d = intraLineDiff("int x = 0;", "int y = 10;");
	EXPECT_EQ(d.left, (std::vector<DiffSpan>{{4, 5}}));
	EXPECT_EQ(d.right, (std::vector<DiffSpan>{{4, 5}, {8, 9}}));
	auto same = intraLineDiff("abc", "abc");
	EXPECT_TRUE(same.left.empty());
	EXPECT_TRUE(same.right.empty());
	auto ins = intraLineDiff("foo()", "foo(bar)");
	EXPECT_TRUE(ins.left.empty());
	EXPECT_EQ(ins.right, (std::vector<DiffSpan>{{4, 7}}));
	// UTF-8: a whole code point is one change.
	auto u = intraLineDiff("a\xCF\x80" "b", "a\xC3\xA9" "b");
	EXPECT_EQ(u.left, (std::vector<DiffSpan>{{1, 3}}));
	EXPECT_EQ(u.right, (std::vector<DiffSpan>{{1, 3}}));
	// Over the limit: prefix / suffix trimming only.
	auto big = intraLineDiff("xAAAAy", "xBBBy", 2);
	EXPECT_EQ(big.left, (std::vector<DiffSpan>{{1, 5}}));
	EXPECT_EQ(big.right, (std::vector<DiffSpan>{{1, 4}}));
}

TEST(DiffViewTest, SideBySidePairsChangedLines)
{
	const auto r = MyersDiff::diffText("a\nint x;\nold\nz\n", "a\nint y;\nz\nnew\n");
	const auto rows = buildSideBySide(r);
	ASSERT_EQ(rows.size(), 5u);
	EXPECT_EQ(rows[0].left.kind, DiffCellKind::Equal);
	EXPECT_EQ(rows[1].left.kind, DiffCellKind::Removed);
	EXPECT_EQ(rows[1].left.text, "int x;");
	EXPECT_EQ(rows[1].right.kind, DiffCellKind::Added);
	EXPECT_EQ(rows[1].right.text, "int y;");
	EXPECT_EQ(rows[1].left.spans, (std::vector<DiffSpan>{{4, 5}}));
	EXPECT_EQ(rows[2].left.text, "old");
	EXPECT_EQ(rows[2].right.kind, DiffCellKind::Placeholder);
	EXPECT_EQ(rows[3].left.line, 3);
	EXPECT_EQ(rows[3].right.line, 2);
	EXPECT_EQ(rows[4].left.kind, DiffCellKind::Placeholder);
	EXPECT_EQ(rows[4].right.text, "new");
	EXPECT_EQ(diffHunkStarts(rows), (std::vector<int>{1, 4}));

	const auto qtRows = buildSideBySide(r, false);
	EXPECT_EQ(qtRows.size(), r.ops.size());
	EXPECT_TRUE(qtRows[1].right.kind == DiffCellKind::Placeholder || qtRows[1].left.kind == DiffCellKind::Placeholder);
}

TEST(DiffViewTest, MostlyDifferentPairsGetNoSpans)
{
	const auto r = MyersDiff::diffText("a\nputs(\"hi\");\nz\n", "a\nreturn count;\nz\n");
	const auto rows = buildSideBySide(r);
	ASSERT_EQ(rows.size(), 3u);
	EXPECT_EQ(rows[1].left.kind, DiffCellKind::Removed);
	EXPECT_EQ(rows[1].right.kind, DiffCellKind::Added);
	EXPECT_TRUE(rows[1].left.spans.empty());
	EXPECT_TRUE(rows[1].right.spans.empty());
	EXPECT_TRUE(buildUnified(r)[1].spans.empty());
}

TEST(DiffViewTest, UnifiedRowsAndStats)
{
	const auto r = MyersDiff::diffText("a\nint x;\nz\n", "a\nint y;\nz\n");
	const auto rows = buildUnified(r);
	ASSERT_EQ(rows.size(), 4u);
	EXPECT_EQ(rows[1].prefix(), '-');
	EXPECT_EQ(rows[2].prefix(), '+');
	EXPECT_EQ(rows[2].spans, (std::vector<DiffSpan>{{4, 5}}));
	EXPECT_EQ(rows[3].leftLine, 2);
	EXPECT_EQ(rows[3].rightLine, 2);
	EXPECT_EQ(diffHunkStarts(rows), (std::vector<int>{1}));
	EXPECT_EQ(diffStatsText(r), "+1  -1  50% similar");
	EXPECT_EQ(diffStatsText(DiffResult{}), "No diff");

	DiffHunkCursor c;
	c.reset({3, 9});
	EXPECT_EQ(c.next(), 3);
	EXPECT_EQ(c.next(), 9);
	EXPECT_EQ(c.next(), 3);
	EXPECT_EQ(c.prev(), 9);
	c.reset({3, 9});
	EXPECT_EQ(c.prev(), 9);
	c.reset({});
	EXPECT_FALSE(c.next().has_value());
}

// ─── Headless UI ─────────────────────────────────────────────────────────────

namespace {

std::string numbered(int n, int changeAt, const std::string& changed)
{
	std::string s;
	for (int i = 0; i < n; ++i)
		s += (i == changeAt ? changed : "    line_" + std::to_string(i) + " = compute(" + std::to_string(i) + ");") + "\n";
	return s;
}

} // namespace

TEST(DiffPanelUiTest, StagesModesNavigationAndCopy)
{
	Headless h({1000.f, 300.f});
	DiffPanel panel(h.ui, h.ui.root());
	int changes = 0;
	panel.onDiffChanged = [&](const DiffResult&) { ++changes; };
	std::string before = numbered(60, -1, "");
	std::string after = before;
	after.replace(after.find("line_5 "), 7, "renamed_5 ");
	after.replace(after.find("compute(40)"), 11, "compute(41)");
	panel.addStage("after_typing", before, after);
	panel.addStage("after_patterns", "int a;\n", "int a;\nint b;\n");
	h.frame();
	EXPECT_EQ(changes, 2);
	EXPECT_EQ(h.ui.text(panel.rightLabel()), "After: after_patterns");
	EXPECT_EQ(h.ui.value(panel.stageCombo()), 2.f);

	// Pick the first stage from the combo with the keyboard.
	h.ui.setFocus(panel.stageCombo(), fuse::ui::FocusReason::Keyboard);
	h.key(fuse::ui::Key::Up);
	EXPECT_EQ(h.ui.text(panel.leftLabel()), "Before: after_typing");
	EXPECT_EQ(panel.currentDiff().linesAdded, 2);
	EXPECT_EQ(panel.statsText(), "+2  -2  93% similar");
	EXPECT_EQ(h.ui.text(panel.statsNode().valid() ? h.ui.firstChild(panel.statsNode()) : panel.statsNode()), "+2");

	// Next / Prev buttons walk the hunks and scroll both panes together.
	EXPECT_LT(panel.visibleRows(), 60);
	h.clickNode(panel.nextButton());
	EXPECT_EQ(panel.cursorRow(), 5);
	h.clickNode(panel.nextButton());
	EXPECT_EQ(panel.cursorRow(), 40);
	EXPECT_EQ(panel.firstVisibleRow(), 38);
	h.clickNode(panel.prevButton());
	EXPECT_EQ(panel.cursorRow(), 5);
	EXPECT_EQ(panel.firstVisibleRow(), 3);

	// Keyboard and wheel on a pane.
	h.clickNode(panel.leftPane());
	h.key(fuse::ui::Key::Home);
	EXPECT_EQ(panel.firstVisibleRow(), 0);
	h.key(fuse::ui::Key::N);
	EXPECT_EQ(panel.cursorRow(), 40);
	h.key(fuse::ui::Key::PageDown);
	EXPECT_GT(panel.firstVisibleRow(), 38);
	const auto r = h.ui.rect(panel.rightPane());
	h.ui.wheel({r.x + 20.f, r.y + 20.f}, {0.f, 2.f});
	h.frame();
	EXPECT_EQ(panel.firstVisibleRow(), std::max(0, static_cast<int>(panel.rowCount()) - panel.visibleRows()) - 6);

	// Unified mode via the combo.
	h.ui.setFocus(panel.modeCombo(), fuse::ui::FocusReason::Keyboard);
	h.key(fuse::ui::Key::Down);
	EXPECT_EQ(panel.viewMode(), DiffPanel::ViewMode::Unified);
	EXPECT_TRUE(h.ui.computed(panel.unifiedPane()).visible);
	EXPECT_EQ(panel.rowCount(), 62u);
	EXPECT_EQ(panel.nextHunk(), 5);
	EXPECT_EQ(panel.unifiedRows()[5].prefix(), '-');
	EXPECT_EQ(panel.unifiedRows()[6].prefix(), '+');

	// Copy button and Ctrl+C on a pane.
	h.clickNode(panel.copyButton());
	std::string clip;
	ASSERT_TRUE(h.ui.clipboardText(clip));
	EXPECT_EQ(clip, panel.unifiedText());
	EXPECT_EQ(clip.substr(0, 20), "--- before\n+++ after");
}

TEST(DiffPanelUiTest, ExportAndRender)
{
	Headless h({1000.f, 320.f});
	DiffPanel panel(h.ui, h.ui.root());
	panel.setDiff(
		"int main(void)\n{\n\tint x = 0;\n\tputs(\"hi\");\n\treturn x;\n}\n",
		"int main(void)\n{\n\tint count = 10;\n\treturn count;\n}\n// generated\n",
		"original vs refined");
	h.frame();
	const auto dir = std::filesystem::temp_directory_path() / "retdec-gui-diff-test";
	std::filesystem::remove_all(dir);
	std::filesystem::create_directories(dir);
	ASSERT_TRUE(panel.exportHtml((dir / "d.html").string()));
	h.clickNode(panel.exportUnifiedButton());
	ASSERT_TRUE(w::fileDialogNavigate(h.ui, panel.fileDialogNode(), dir.string()));
	h.frame();
	h.clickNode(h.findByText("Save"));
	std::ifstream in(dir / "diff.patch");
	std::stringstream ss;
	ss << in.rdbuf();
	EXPECT_EQ(ss.str(), panel.unifiedText());
	std::filesystem::remove_all(dir);
	h.frame();

	panel.nextHunk();
	EXPECT_GT(h.render("diff_panel_side_by_side.png"), 1000u);
	panel.setViewMode(DiffPanel::ViewMode::Unified);
	EXPECT_GT(h.render("diff_panel_unified.png"), 1000u);
}
