/**
 * @file include/retdec/gui/panels/diff_panel.h
 * @brief Qt GUI: Diff/Compare View for Before-After Analysis — Stage 56.
 *
 * Side-by-side comparison panel showing the decompiled output before and after
 * a set of semantic recovery passes, with Myers-diff line highlighting.
 *
 * ## Features
 *
 *   - **Side-by-side diff**: two synchronised `QPlainTextEdit` panes (left =
 *     before, right = after) with line numbers.
 *   - **Myers diff algorithm**: O((N+M)D) diff on lines; produces a minimal
 *     edit script of Insert / Delete / Equal operations.
 *   - **Colour-coded highlighting**:
 *       Removed line  — red tint background  (#3d1212)
 *       Added line    — green tint            (#122d12)
 *       Changed block — yellow tint           (#2d2d12) (equal but surrounded by changes)
 *       Equal line    — default background
 *   - **Stage selector**: dropdown to choose which recovery stage's delta to
 *     show (e.g., "after_typing", "after_patterns", "after_concurrency").
 *   - **Statistics bar**: N lines added / M lines removed / K% similarity.
 *   - **Scroll synchronisation**: vertical scroll in one pane scrolls the other.
 *   - **Navigation**: "Next diff" / "Previous diff" buttons jump to nearest
 *     changed hunk.
 *   - **Export**: save diff as unified diff text or HTML report.
 *   - **Copy diff**: copy the unified diff to clipboard.
 */

#ifndef RETDEC_GUI_PANELS_DIFF_PANEL_H
#define RETDEC_GUI_PANELS_DIFF_PANEL_H

#include "retdec/gui/panels/panel_base.h"

#include <QPlainTextEdit>
#include <QTextCharFormat>
#include <QWidget>

#include <cstdint>
#include <string>
#include <vector>

QT_BEGIN_NAMESPACE
class QComboBox;
class QLabel;
class QScrollBar;
class QSplitter;
class QToolButton;
QT_END_NAMESPACE

namespace retdec::gui::panels {

// ─── Myers diff data structures ───────────────────────────────────────────────

enum class DiffOpKind { Equal, Insert, Delete };

struct DiffOp {
    DiffOpKind  kind   = DiffOpKind::Equal;
    std::string line;
    int         leftLine  = -1;  ///< 0-based line number in the left (before) file
    int         rightLine = -1;  ///< 0-based line number in the right (after) file
};

/**
 * @brief Result of a complete Myers diff.
 */
struct DiffResult {
    std::vector<DiffOp> ops;
    int linesAdded   = 0;
    int linesRemoved = 0;
    int linesEqual   = 0;
    double similarity = 0.0;  ///< 0.0 – 1.0

    bool isEmpty() const { return ops.empty(); }

    /** @brief Format as a unified diff (--- / +++ / @@ hunks). */
    std::string toUnifiedDiff(const std::string& leftName  = "before",
                               const std::string& rightName = "after",
                               int context = 3) const;

    /** @brief Format as an HTML report. */
    std::string toHtml() const;
};

// ─── MyersDiff ────────────────────────────────────────────────────────────────

/**
 * @brief Computes a minimal Myers diff between two sequences of lines.
 *
 * Complexity: O((N+M)*D) time, O((N+M)) space where D = edit distance.
 */
class MyersDiff {
public:
    /**
     * @brief Diff two texts split into lines.
     */
    static DiffResult diff(const std::vector<std::string>& left,
                            const std::vector<std::string>& right);

    /**
     * @brief Convenience: split text into lines and diff.
     */
    static DiffResult diffText(const std::string& leftText,
                                const std::string& rightText);

private:
    struct Snake {
        int x, y, u, v;  // (x,y) start → (u,v) end
    };

    static Snake midpoint(const std::vector<std::string>& a,
                           const std::vector<std::string>& b,
                           int aLo, int aHi, int bLo, int bHi);

    static void backtrack(const std::vector<std::string>& a,
                           const std::vector<std::string>& b,
                           int aLo, int aHi, int bLo, int bHi,
                           std::vector<DiffOp>& out);
};

// ─── DiffLineNumberArea ───────────────────────────────────────────────────────

class DiffPane;

/**
 * @brief Line-number gutter for a DiffPane.
 */
class DiffLineNumberArea : public QWidget {
    Q_OBJECT
public:
    explicit DiffLineNumberArea(DiffPane* editor);
    QSize sizeHint() const override;

protected:
    void paintEvent(QPaintEvent* event) override;

private:
    DiffPane* editor_;
};

// ─── DiffPane ─────────────────────────────────────────────────────────────────

/**
 * @brief Read-only QPlainTextEdit with diff highlighting and a line-number gutter.
 *
 * Stores per-line colour data externally via setLineColors().
 */
class DiffPane : public QPlainTextEdit {
    Q_OBJECT
public:
    explicit DiffPane(QWidget* parent = nullptr);

    /**
     * @brief Set per-line background colours.
     *        Index = 0-based visible line number.
     *        QColor() (invalid) = default background.
     */
    void setLineColors(const std::vector<QColor>& colors);

    /**
     * @brief Set the text content and re-apply colours.
     */
    void setContent(const QString& text, const std::vector<QColor>& colors);

    int lineNumberAreaWidth() const;
    void lineNumberAreaPaintEvent(QPaintEvent* event);

    /** @brief Scroll to the given 0-based line. */
    void scrollToLine(int line);

protected:
    void resizeEvent(QResizeEvent* event) override;

private slots:
    void updateLineNumberAreaWidth(int newBlockCount);
    void highlightCurrentLine();
    void updateLineNumberArea(const QRect& rect, int dy);

private:
    DiffLineNumberArea*  lineNumberArea_;
    std::vector<QColor>  lineColors_;
};

// ─── DiffStats ────────────────────────────────────────────────────────────────

struct DiffStats {
    int    added     = 0;
    int    removed   = 0;
    int    unchanged = 0;
    double similarity = 0.0;
};

// ─── DiffPanel ────────────────────────────────────────────────────────────────

/**
 * @brief Side-by-side diff panel.
 *
 * Public API:
 *   setDiff(before, after)   — compute and display diff
 *   addStage(name, before, after) — register a named recovery stage delta
 *   clear()                  — reset all content
 *
 * Signals:
 *   diffChanged(DiffResult)  — emitted when the displayed diff changes
 */
class DiffPanel : public PanelBase {
    Q_OBJECT
public:
    explicit DiffPanel(QWidget* parent = nullptr);

    /**
     * @brief Set the before/after text and compute + display the diff.
     */
    void setDiff(const QString& before, const QString& after,
                 const QString& stageName = "");

    /**
     * @brief Register a named recovery-stage diff for the stage selector.
     */
    void addStage(const QString& name,
                  const QString& before, const QString& after);

    void clear() override;

    const DiffResult& currentDiff() const { return currentDiff_; }

signals:
    void diffChanged(const DiffResult& result);

private slots:
    void onStageSelected(int index);
    void onNextDiff();
    void onPrevDiff();
    void onExportUnified();
    void onExportHtml();
    void onCopyDiff();
    void onLeftScrollChanged(int value);
    void onRightScrollChanged(int value);

private:
    void setupUI();
    void applyDiff(const DiffResult& diff);
    void buildLineViews(const DiffResult& diff,
                         std::vector<QString>& leftLines,
                         std::vector<QColor>&  leftColors,
                         std::vector<QString>& rightLines,
                         std::vector<QColor>&  rightColors) const;
    void updateStats();
    int  nextHunk(int fromLine, bool forward) const;

    // ── UI components ────────────────────────────────────────────────────────
    QComboBox*    stageCombo_    = nullptr;
    QLabel*       statsLabel_    = nullptr;
    QToolButton*  prevBtn_       = nullptr;
    QToolButton*  nextBtn_       = nullptr;
    QToolButton*  exportUBtn_    = nullptr;
    QToolButton*  exportHBtn_    = nullptr;
    QToolButton*  copyBtn_       = nullptr;
    QLabel*       leftLabel_     = nullptr;
    QLabel*       rightLabel_    = nullptr;
    DiffPane*     leftPane_      = nullptr;
    DiffPane*     rightPane_     = nullptr;
    QSplitter*    splitter_      = nullptr;

    // ── State ────────────────────────────────────────────────────────────────
    DiffResult    currentDiff_;
    int           currentHunk_   = 0;

    struct StageDiff {
        QString name, before, after;
    };
    std::vector<StageDiff> stages_;

    // ── Scroll sync guard ────────────────────────────────────────────────────
    bool syncingScroll_ = false;

    // ── Hunk line numbers (for navigation) ──────────────────────────────────
    std::vector<int> hunkLines_;  ///< first line of each diff hunk in right pane
};

} // namespace retdec::gui::panels

#endif // RETDEC_GUI_PANELS_DIFF_PANEL_H
