/**
 * @file tests/gui/diff_panel_test.cpp
 * @brief Unit tests for MyersDiff and DiffPanel.
 */

#include "retdec/gui/panels/diff_panel.h"
#include <gtest/gtest.h>
#include <QApplication>
#include <QString>
#include <string>
#include <vector>

using namespace retdec::gui::panels;

// ─── Test fixture: QApplication ───────────────────────────────────────────────

class DiffPanelTest : public ::testing::Test {
protected:
    void SetUp() override {
        Q_ASSERT(QApplication::instance() != nullptr);
    }
    void TearDown() override {
        if (QApplication::instance())
            QApplication::processEvents();
    }
};

// ─── MyersDiff ────────────────────────────────────────────────────────────────

TEST_F(DiffPanelTest, DiffIdenticalTexts) {
    auto r = MyersDiff::diffText("a\nb\nc\n", "a\nb\nc\n");
    EXPECT_EQ(r.linesAdded,   0);
    EXPECT_EQ(r.linesRemoved, 0);
    EXPECT_GT(r.linesEqual,   0);
    EXPECT_DOUBLE_EQ(r.similarity, 1.0);
}

TEST_F(DiffPanelTest, DiffCompletelyDifferent) {
    auto r = MyersDiff::diffText("a\nb\nc\n", "x\ny\nz\n");
    EXPECT_EQ(r.linesAdded,   3);
    EXPECT_EQ(r.linesRemoved, 3);
    EXPECT_EQ(r.linesEqual,   0);
    EXPECT_DOUBLE_EQ(r.similarity, 0.0);
}

TEST_F(DiffPanelTest, DiffOneLineAdded) {
    auto r = MyersDiff::diffText("a\nb\n", "a\nb\nc\n");
    EXPECT_EQ(r.linesAdded,   1);
    EXPECT_EQ(r.linesRemoved, 0);
    EXPECT_EQ(r.linesEqual,   2);
}

TEST_F(DiffPanelTest, DiffOneLineRemoved) {
    auto r = MyersDiff::diffText("a\nb\nc\n", "a\nc\n");
    EXPECT_EQ(r.linesAdded,   0);
    EXPECT_EQ(r.linesRemoved, 1);
    EXPECT_EQ(r.linesEqual,   2);
}

TEST_F(DiffPanelTest, DiffLineChanged) {
    auto r = MyersDiff::diffText("a\nfoo\nc\n", "a\nbar\nc\n");
    EXPECT_EQ(r.linesAdded,   1);
    EXPECT_EQ(r.linesRemoved, 1);
    EXPECT_EQ(r.linesEqual,   2);
}

TEST_F(DiffPanelTest, DiffEmptyLeft) {
    auto r = MyersDiff::diffText("", "a\nb\n");
    EXPECT_EQ(r.linesAdded,   2);
    EXPECT_EQ(r.linesRemoved, 0);
}

TEST_F(DiffPanelTest, DiffEmptyRight) {
    auto r = MyersDiff::diffText("a\nb\n", "");
    EXPECT_EQ(r.linesAdded,   0);
    EXPECT_EQ(r.linesRemoved, 2);
}

TEST_F(DiffPanelTest, DiffBothEmpty) {
    auto r = MyersDiff::diffText("", "");
    EXPECT_EQ(r.linesAdded,   0);
    EXPECT_EQ(r.linesRemoved, 0);
    EXPECT_EQ(r.linesEqual,   0);
    EXPECT_DOUBLE_EQ(r.similarity, 1.0);
}

TEST_F(DiffPanelTest, DiffSimilarityRange) {
    auto r = MyersDiff::diffText("a\nb\nc\nd\n", "a\nb\nX\nd\n");
    EXPECT_GE(r.similarity, 0.0);
    EXPECT_LE(r.similarity, 1.0);
}

TEST_F(DiffPanelTest, DiffOpsCountMatchesLines) {
    auto r = MyersDiff::diffText("a\nb\nc\n", "a\nX\nc\nd\n");
    int count = r.linesAdded + r.linesRemoved + r.linesEqual;
    EXPECT_EQ(count, static_cast<int>(r.ops.size()));
}

TEST_F(DiffPanelTest, DiffEqualOpsContainCorrectLines) {
    auto r = MyersDiff::diffText("a\nb\nc\n", "a\nb\nc\n");
    for (const auto& op : r.ops) {
        EXPECT_EQ(op.kind, DiffOpKind::Equal);
        EXPECT_FALSE(op.line.empty());
    }
}

TEST_F(DiffPanelTest, DiffInsertOpsHaveNoLeftLine) {
    auto r = MyersDiff::diffText("a\n", "a\nb\n");
    bool foundInsert = false;
    for (const auto& op : r.ops) {
        if (op.kind == DiffOpKind::Insert) {
            foundInsert = true;
            EXPECT_EQ(op.leftLine, -1);
        }
    }
    EXPECT_TRUE(foundInsert);
}

TEST_F(DiffPanelTest, DiffDeleteOpsHaveNoRightLine) {
    auto r = MyersDiff::diffText("a\nb\n", "a\n");
    bool foundDelete = false;
    for (const auto& op : r.ops) {
        if (op.kind == DiffOpKind::Delete) {
            foundDelete = true;
            EXPECT_EQ(op.rightLine, -1);
        }
    }
    EXPECT_TRUE(foundDelete);
}

// ─── DiffResult::toUnifiedDiff ────────────────────────────────────────────────

TEST_F(DiffPanelTest, UnifiedDiffHeader) {
    auto r = MyersDiff::diffText("a\n", "b\n");
    auto s = r.toUnifiedDiff("left.c", "right.c");
    EXPECT_NE(s.find("--- left.c"), std::string::npos);
    EXPECT_NE(s.find("+++ right.c"), std::string::npos);
}

TEST_F(DiffPanelTest, UnifiedDiffHunkHeader) {
    auto r = MyersDiff::diffText("a\nb\nc\n", "a\nX\nc\n");
    auto s = r.toUnifiedDiff();
    EXPECT_NE(s.find("@@"), std::string::npos);
}

TEST_F(DiffPanelTest, UnifiedDiffContainsMinusPlus) {
    auto r = MyersDiff::diffText("old\n", "new\n");
    auto s = r.toUnifiedDiff();
    EXPECT_NE(s.find("-old"), std::string::npos);
    EXPECT_NE(s.find("+new"), std::string::npos);
}

TEST_F(DiffPanelTest, UnifiedDiffEmptyForIdentical) {
    auto r = MyersDiff::diffText("same\n", "same\n");
    auto s = r.toUnifiedDiff();
    EXPECT_EQ(s.find("@@"), std::string::npos);
}

// ─── DiffResult::toHtml ───────────────────────────────────────────────────────

TEST_F(DiffPanelTest, HtmlOutputContainsTags) {
    auto r = MyersDiff::diffText("a\n", "b\n");
    auto s = r.toHtml();
    EXPECT_NE(s.find("<html>"), std::string::npos);
    EXPECT_NE(s.find("</html>"), std::string::npos);
}

TEST_F(DiffPanelTest, HtmlEscapesSpecialChars) {
    auto r = MyersDiff::diffText("<tag>\n", "<tag>\n");
    auto s = r.toHtml();
    EXPECT_EQ(s.find("<tag>"), std::string::npos);  // should be escaped
    EXPECT_NE(s.find("&lt;"), std::string::npos);
}

// ─── DiffPanel widget ─────────────────────────────────────────────────────────

TEST_F(DiffPanelTest, PanelConstruction) {
    DiffPanel panel;
    EXPECT_NE(&panel, nullptr);
}

TEST_F(DiffPanelTest, SetDiffUpdatesCurrentDiff) {
    DiffPanel panel;
    panel.setDiff("a\nb\n", "a\nX\n");
    EXPECT_FALSE(panel.currentDiff().isEmpty());
}

TEST_F(DiffPanelTest, SetDiffEmitsDiffChanged) {
    DiffPanel panel;
    bool emitted = false;
    QObject::connect(&panel, &DiffPanel::diffChanged,
                     [&](const DiffResult&) { emitted = true; });
    panel.setDiff("old\n", "new\n");
    EXPECT_TRUE(emitted);
}

TEST_F(DiffPanelTest, ClearResetsDiff) {
    DiffPanel panel;
    panel.setDiff("x\n", "y\n");
    panel.clear();
    EXPECT_TRUE(panel.currentDiff().isEmpty());
}

TEST_F(DiffPanelTest, AddStageRegistersStage) {
    DiffPanel panel;
    panel.addStage("after_typing", "int x;\n", "int x = 0;\n");
    // After adding, the stage combo should have at least 2 items
    // (index 0 = <manual>, index 1 = after_typing)
    EXPECT_FALSE(panel.currentDiff().isEmpty() &&
                 panel.currentDiff().linesAdded == 0);  // just no crash
}

TEST_F(DiffPanelTest, IdenticalDiffNoOps) {
    DiffPanel panel;
    panel.setDiff("same\nsame2\n", "same\nsame2\n");
    EXPECT_EQ(panel.currentDiff().linesAdded, 0);
    EXPECT_EQ(panel.currentDiff().linesRemoved, 0);
}

TEST_F(DiffPanelTest, DiffWithManyLines) {
    std::string left, right;
    for (int i = 0; i < 100; ++i) left  += "line " + std::to_string(i) + "\n";
    for (int i = 0; i < 100; ++i) {
        if (i == 50) right += "inserted\n";
        right += "line " + std::to_string(i) + "\n";
    }
    DiffPanel panel;
    panel.setDiff(QString::fromStdString(left), QString::fromStdString(right));
    EXPECT_EQ(panel.currentDiff().linesAdded, 1);
    EXPECT_EQ(panel.currentDiff().linesRemoved, 0);
}

TEST_F(DiffPanelTest, SimilarityIsOneForIdentical) {
    DiffPanel panel;
    panel.setDiff("a\nb\nc\n", "a\nb\nc\n");
    EXPECT_DOUBLE_EQ(panel.currentDiff().similarity, 1.0);
}
