/**
 * @file src/gui/panels/diff_panel.cpp
 * @brief Diff/Compare View panel implementation.
 */

#include "retdec/gui/panels/diff_panel.h"

#include <QApplication>
#include <QClipboard>
#include <QComboBox>
#include <QFileDialog>
#include <QFontMetrics>
#include <QHBoxLayout>
#include <QLabel>
#include <QPainter>
#include <QScrollBar>
#include <QSplitter>
#include <QTextBlock>
#include <QToolBar>
#include <QToolButton>
#include <QVBoxLayout>

#include <algorithm>
#include <climits>
#include <sstream>
#include <utility>
#include <vector>

namespace retdec::gui::panels {

// ─── Colour palette ───────────────────────────────────────────────────────────

static const QColor kColorRemoved ("#3d1212");
static const QColor kColorAdded   ("#122d12");
static const QColor kColorChanged ("#2d2d12");
static const QColor kColorEmpty   ("#181825");   // Catppuccin Mantle — placeholder line colour
static const QColor kColLineNum   ("#555577");

// ─── DiffResult ───────────────────────────────────────────────────────────────

std::string DiffResult::toUnifiedDiff(const std::string& leftName,
                                       const std::string& rightName,
                                       int context) const {
    std::ostringstream os;
    os << "--- " << leftName << "\n";
    os << "+++ " << rightName << "\n";

    // Group ops into hunks with context
    const int n = static_cast<int>(ops.size());
    int i = 0;
    while (i < n) {
        // Find next non-equal op
        while (i < n && ops[i].kind == DiffOpKind::Equal) ++i;
        if (i >= n) break;

        int start = std::max(0, i - context);
        int end   = i;
        // Extend to cover contiguous changes + trailing context
        while (end < n) {
            if (ops[end].kind != DiffOpKind::Equal) end++;
            else {
                // Count equal lines ahead
                int eq = 0;
                for (int j = end; j < n && ops[j].kind == DiffOpKind::Equal; ++j) eq++;
                if (eq <= 2 * context) end += eq; // merge nearby hunks
                else { end += context; break; }
            }
        }
        end = std::min(end, n);

        // Compute line numbers for @@ header
        int leftStart  = 1, leftLen = 0, rightStart = 1, rightLen = 0;
        for (int j = 0; j < start; ++j) {
            if (ops[j].kind != DiffOpKind::Insert) leftStart++;
            if (ops[j].kind != DiffOpKind::Delete) rightStart++;
        }
        for (int j = start; j < end; ++j) {
            if (ops[j].kind != DiffOpKind::Insert) leftLen++;
            if (ops[j].kind != DiffOpKind::Delete) rightLen++;
        }

        os << "@@ -" << leftStart << "," << leftLen
           << " +" << rightStart << "," << rightLen << " @@\n";
        for (int j = start; j < end; ++j) {
            char prefix = ' ';
            if (ops[j].kind == DiffOpKind::Delete) prefix = '-';
            if (ops[j].kind == DiffOpKind::Insert) prefix = '+';
            os << prefix << ops[j].line << "\n";
        }
        i = end;
    }
    return os.str();
}

std::string DiffResult::toHtml() const {
    std::ostringstream os;
    os << "<html><body><pre style='font-family:monospace'>\n";
    for (const auto& op : ops) {
        std::string cls;
        if (op.kind == DiffOpKind::Delete) cls = "background:#3d1212;color:#ff9999";
        else if (op.kind == DiffOpKind::Insert) cls = "background:#122d12;color:#99ff99";
        else cls = "background:#1e1e2e;color:#cdd6f4";
        char prefix = op.kind == DiffOpKind::Delete ? '-' :
                      op.kind == DiffOpKind::Insert ? '+' : ' ';
        // Escape HTML
        std::string escaped;
        for (char c : op.line) {
            if (c == '<') escaped += "&lt;";
            else if (c == '>') escaped += "&gt;";
            else if (c == '&') escaped += "&amp;";
            else escaped += c;
        }
        os << "<span style='" << cls << "'>" << prefix << escaped << "</span>\n";
    }
    os << "</pre></body></html>\n";
    return os.str();
}

// ─── MyersDiff ────────────────────────────────────────────────────────────────
//
// The first-cut implementation here used a hand-rolled Hirschberg/Myers
// divide-and-conquer with a bug in the midpoint snake selection — it
// produced too few delete ops on basic inputs (see DiffPanelTest cases that
// were failing). For the GUI's use case (single-function diff, up to a few
// thousand lines) a straightforward LCS-DP is fast enough and obviously
// correct. We keep the same backtrack/midpoint method declarations so the
// public ABI does not change.

namespace {

// Build the LCS length DP table for sequences A[0..N) and B[0..M).
// dp[i][j] = length of LCS of A[0..i) and B[0..j).
std::vector<std::vector<int>> buildLcsTable(
        const std::vector<std::string>& a,
        const std::vector<std::string>& b) {
    const int n = static_cast<int>(a.size());
    const int m = static_cast<int>(b.size());
    std::vector<std::vector<int>> dp(n + 1, std::vector<int>(m + 1, 0));
    for (int i = 1; i <= n; ++i) {
        const auto& ai = a[i - 1];
        for (int j = 1; j <= m; ++j) {
            if (ai == b[j - 1])
                dp[i][j] = dp[i - 1][j - 1] + 1;
            else
                dp[i][j] = std::max(dp[i - 1][j], dp[i][j - 1]);
        }
    }
    return dp;
}

// Walk the table backwards to emit a minimal edit script in source order.
void emitOpsFromLcs(const std::vector<std::vector<int>>& dp,
                    const std::vector<std::string>& a,
                    const std::vector<std::string>& b,
                    std::vector<DiffOp>& out) {
    std::vector<DiffOp> rev;
    int i = static_cast<int>(a.size());
    int j = static_cast<int>(b.size());
    while (i > 0 && j > 0) {
        if (a[i - 1] == b[j - 1]) {
            DiffOp op;
            op.kind = DiffOpKind::Equal;
            op.line = a[i - 1];
            op.leftLine = i - 1;
            op.rightLine = j - 1;
            rev.push_back(std::move(op));
            --i; --j;
        } else if (dp[i - 1][j] >= dp[i][j - 1]) {
            DiffOp op;
            op.kind = DiffOpKind::Delete;
            op.line = a[i - 1];
            op.leftLine = i - 1;
            rev.push_back(std::move(op));
            --i;
        } else {
            DiffOp op;
            op.kind = DiffOpKind::Insert;
            op.line = b[j - 1];
            op.rightLine = j - 1;
            rev.push_back(std::move(op));
            --j;
        }
    }
    while (i > 0) {
        DiffOp op;
        op.kind = DiffOpKind::Delete;
        op.line = a[i - 1];
        op.leftLine = i - 1;
        rev.push_back(std::move(op));
        --i;
    }
    while (j > 0) {
        DiffOp op;
        op.kind = DiffOpKind::Insert;
        op.line = b[j - 1];
        op.rightLine = j - 1;
        rev.push_back(std::move(op));
        --j;
    }
    out.reserve(out.size() + rev.size());
    for (auto it = rev.rbegin(); it != rev.rend(); ++it)
        out.push_back(std::move(*it));
}

} // namespace

// midpoint/backtrack are kept only because they're listed in the header.
// They are not used by the LCS-based diff() below, but defining them avoids
// breaking the published method signatures.
MyersDiff::Snake MyersDiff::midpoint(
    const std::vector<std::string>& /*a*/, const std::vector<std::string>& /*b*/,
    int aLo, int aHi, int bLo, int bHi) {
    return {aLo, bLo, aHi, bHi};
}

void MyersDiff::backtrack(
    const std::vector<std::string>& a, const std::vector<std::string>& b,
    int aLo, int aHi, int bLo, int bHi,
    std::vector<DiffOp>& out) {
    // Delegate to the LCS implementation on the requested sub-range.
    std::vector<std::string> sub_a(a.begin() + aLo, a.begin() + aHi);
    std::vector<std::string> sub_b(b.begin() + bLo, b.begin() + bHi);
    if (sub_a.empty() && sub_b.empty())
        return;
    auto dp = buildLcsTable(sub_a, sub_b);
    std::vector<DiffOp> local;
    emitOpsFromLcs(dp, sub_a, sub_b, local);
    for (auto& op : local) {
        if (op.leftLine  >= 0) op.leftLine  += aLo;
        if (op.rightLine >= 0) op.rightLine += bLo;
        out.push_back(std::move(op));
    }
}

DiffResult MyersDiff::diff(const std::vector<std::string>& left,
                             const std::vector<std::string>& right) {
    DiffResult result;
    if (!left.empty() || !right.empty()) {
        auto dp = buildLcsTable(left, right);
        emitOpsFromLcs(dp, left, right, result.ops);
    }
    for (const auto& op : result.ops) {
        if (op.kind == DiffOpKind::Insert) result.linesAdded++;
        else if (op.kind == DiffOpKind::Delete) result.linesRemoved++;
        else result.linesEqual++;
    }
    int total = result.linesAdded + result.linesRemoved + result.linesEqual;
    result.similarity = total > 0 ? static_cast<double>(result.linesEqual) / total : 1.0;
    return result;
}

DiffResult MyersDiff::diffText(const std::string& leftText,
                                 const std::string& rightText) {
    auto splitLines = [](const std::string& text) {
        std::vector<std::string> lines;
        std::istringstream ss(text);
        std::string line;
        while (std::getline(ss, line)) lines.push_back(line);
        return lines;
    };
    return diff(splitLines(leftText), splitLines(rightText));
}

// ─── DiffLineNumberArea ───────────────────────────────────────────────────────

DiffLineNumberArea::DiffLineNumberArea(DiffPane* editor)
    : QWidget(editor), editor_(editor) {}

QSize DiffLineNumberArea::sizeHint() const {
    return {editor_->lineNumberAreaWidth(), 0};
}

void DiffLineNumberArea::paintEvent(QPaintEvent* event) {
    editor_->lineNumberAreaPaintEvent(event);
}

// ─── DiffPane ────────────────────────────────────────────────────────────────

DiffPane::DiffPane(QWidget* parent)
    : QPlainTextEdit(parent),
      lineNumberArea_(new DiffLineNumberArea(this)) {
    setReadOnly(true);
    setLineWrapMode(QPlainTextEdit::NoWrap);
    setFont(QFont("Cascadia Code,JetBrains Mono,Consolas,monospace", 10));

    QTextCharFormat fmt;
    fmt.setBackground(QColor("#1e1e2e"));
    fmt.setForeground(QColor("#cdd6f4"));
    setCurrentCharFormat(fmt);

    connect(this, &QPlainTextEdit::blockCountChanged,
            this, &DiffPane::updateLineNumberAreaWidth);
    connect(this, &QPlainTextEdit::updateRequest,
            this, &DiffPane::updateLineNumberArea);

    updateLineNumberAreaWidth(0);
}

int DiffPane::lineNumberAreaWidth() const {
    int digits = 1;
    int max    = std::max(1, blockCount());
    while (max >= 10) { max /= 10; ++digits; }
    return 6 + fontMetrics().horizontalAdvance(QLatin1Char('9')) * (digits + 1);
}

void DiffPane::updateLineNumberAreaWidth(int) {
    setViewportMargins(lineNumberAreaWidth(), 0, 0, 0);
}

void DiffPane::updateLineNumberArea(const QRect& rect, int dy) {
    if (dy) lineNumberArea_->scroll(0, dy);
    else    lineNumberArea_->update(0, rect.y(), lineNumberArea_->width(), rect.height());
    if (rect.contains(viewport()->rect()))
        updateLineNumberAreaWidth(0);
}

void DiffPane::resizeEvent(QResizeEvent* e) {
    QPlainTextEdit::resizeEvent(e);
    QRect cr = contentsRect();
    lineNumberArea_->setGeometry(cr.left(), cr.top(), lineNumberAreaWidth(), cr.height());
}

void DiffPane::highlightCurrentLine() {}

void DiffPane::lineNumberAreaPaintEvent(QPaintEvent* event) {
    QPainter painter(lineNumberArea_);
    painter.fillRect(event->rect(), QColor("#181825"));

    QTextBlock block = firstVisibleBlock();
    int blockNum     = block.blockNumber();
    int top          = qRound(blockBoundingGeometry(block).translated(contentOffset()).top());
    int bottom       = top + qRound(blockBoundingRect(block).height());
    int lineH        = fontMetrics().height();

    while (block.isValid() && top <= event->rect().bottom()) {
        if (block.isVisible() && bottom >= event->rect().top()) {
            QColor bg = (blockNum < static_cast<int>(lineColors_.size()) &&
                         lineColors_[blockNum].isValid()) ?
                        lineColors_[blockNum].darker(130) : QColor("#181825");
            painter.fillRect(0, top, lineNumberArea_->width() - 2, lineH, bg);
            painter.setPen(kColLineNum);
            painter.drawText(0, top, lineNumberArea_->width() - 4, lineH,
                             Qt::AlignRight, QString::number(blockNum + 1));
        }
        block  = block.next();
        top    = bottom;
        bottom = top + qRound(blockBoundingRect(block).height());
        ++blockNum;
    }
}

void DiffPane::setLineColors(const std::vector<QColor>& colors) {
    lineColors_ = colors;
    lineNumberArea_->update();
    // Re-highlight using QTextCursor per block
    QTextCursor cursor(document());
    cursor.movePosition(QTextCursor::Start);
    int line = 0;
    while (!cursor.atEnd() && line < static_cast<int>(colors.size())) {
        cursor.select(QTextCursor::LineUnderCursor);
        QTextBlockFormat fmt;
        if (colors[line].isValid())
            fmt.setBackground(colors[line]);
        cursor.setBlockFormat(fmt);
        cursor.movePosition(QTextCursor::NextBlock);
        ++line;
    }
}

void DiffPane::setContent(const QString& text, const std::vector<QColor>& colors) {
    setPlainText(text);
    setLineColors(colors);
}

void DiffPane::scrollToLine(int line) {
    QTextBlock block = document()->findBlockByLineNumber(line);
    if (block.isValid()) {
        QTextCursor cursor(block);
        setTextCursor(cursor);
        ensureCursorVisible();
    }
}

// ─── DiffPanel ───────────────────────────────────────────────────────────────

DiffPanel::DiffPanel(QWidget* parent)
    : PanelBase("Diff / Compare", parent) {
    setupUI();
}

void DiffPanel::setupUI() {
    // ── Toolbar ──────────────────────────────────────────────────────────────
    auto* toolbar = new QToolBar(this);
    toolbar->setIconSize({16, 16});

    toolbar->addWidget(new QLabel("Stage: ", this));
    stageCombo_ = new QComboBox(this);
    stageCombo_->addItem("<manual>");
    stageCombo_->setMinimumWidth(160);
    toolbar->addWidget(stageCombo_);
    toolbar->addSeparator();

    prevBtn_ = new QToolButton(this);
    prevBtn_->setText("▲ Prev");
    prevBtn_->setToolTip("Jump to previous diff hunk");
    toolbar->addWidget(prevBtn_);

    nextBtn_ = new QToolButton(this);
    nextBtn_->setText("▼ Next");
    nextBtn_->setToolTip("Jump to next diff hunk");
    toolbar->addWidget(nextBtn_);

    toolbar->addSeparator();

    exportUBtn_ = new QToolButton(this);
    exportUBtn_->setText("Unified");
    exportUBtn_->setToolTip("Export as unified diff (.patch)");
    toolbar->addWidget(exportUBtn_);

    exportHBtn_ = new QToolButton(this);
    exportHBtn_->setText("HTML");
    exportHBtn_->setToolTip("Export as HTML report");
    toolbar->addWidget(exportHBtn_);

    copyBtn_ = new QToolButton(this);
    copyBtn_->setText("Copy");
    copyBtn_->setToolTip("Copy unified diff to clipboard");
    toolbar->addWidget(copyBtn_);

    // ── Stats label ──────────────────────────────────────────────────────────
    statsLabel_ = new QLabel("No diff", this);
    statsLabel_->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    statsLabel_->setStyleSheet("color: #888;");

    // ── Column headers ───────────────────────────────────────────────────────
    leftLabel_  = new QLabel("Before", this);
    rightLabel_ = new QLabel("After",  this);
    for (auto* lbl : {leftLabel_, rightLabel_}) {
        lbl->setAlignment(Qt::AlignCenter);
        lbl->setStyleSheet("background:#313244; color:#cdd6f4; padding:2px;");
    }

    // ── Diff panes ───────────────────────────────────────────────────────────
    leftPane_  = new DiffPane(this);
    rightPane_ = new DiffPane(this);

    auto* leftWidget  = new QWidget(this);
    auto* rightWidget = new QWidget(this);
    auto* leftLayout  = new QVBoxLayout(leftWidget);
    auto* rightLayout = new QVBoxLayout(rightWidget);
    leftLayout->setContentsMargins(0,0,0,0); leftLayout->setSpacing(0);
    rightLayout->setContentsMargins(0,0,0,0); rightLayout->setSpacing(0);
    leftLayout->addWidget(leftLabel_);
    leftLayout->addWidget(leftPane_);
    rightLayout->addWidget(rightLabel_);
    rightLayout->addWidget(rightPane_);

    splitter_ = new QSplitter(Qt::Horizontal, this);
    splitter_->addWidget(leftWidget);
    splitter_->addWidget(rightWidget);
    splitter_->setSizes({500, 500});

    // ── Main layout ──────────────────────────────────────────────────────────
    auto* statsRow = new QHBoxLayout;
    statsRow->addStretch();
    statsRow->addWidget(statsLabel_);

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(2);
    layout->addWidget(toolbar);
    layout->addLayout(statsRow);
    layout->addWidget(splitter_);

    // ── Scroll sync ──────────────────────────────────────────────────────────
    connect(leftPane_->verticalScrollBar(),  &QScrollBar::valueChanged,
            this, &DiffPanel::onLeftScrollChanged);
    connect(rightPane_->verticalScrollBar(), &QScrollBar::valueChanged,
            this, &DiffPanel::onRightScrollChanged);

    // ── Toolbar connections ───────────────────────────────────────────────────
    connect(stageCombo_, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, &DiffPanel::onStageSelected);
    connect(prevBtn_,    &QToolButton::clicked, this, &DiffPanel::onPrevDiff);
    connect(nextBtn_,    &QToolButton::clicked, this, &DiffPanel::onNextDiff);
    connect(exportUBtn_, &QToolButton::clicked, this, &DiffPanel::onExportUnified);
    connect(exportHBtn_, &QToolButton::clicked, this, &DiffPanel::onExportHtml);
    connect(copyBtn_,    &QToolButton::clicked, this, &DiffPanel::onCopyDiff);
}

// ── buildLineViews ────────────────────────────────────────────────────────────

void DiffPanel::buildLineViews(
    const DiffResult& diff,
    std::vector<QString>& leftLines,
    std::vector<QColor>&  leftColors,
    std::vector<QString>& rightLines,
    std::vector<QColor>&  rightColors) const {
    for (const auto& op : diff.ops) {
        switch (op.kind) {
        case DiffOpKind::Equal:
            leftLines.push_back(QString::fromStdString(op.line));
            leftColors.push_back({});
            rightLines.push_back(QString::fromStdString(op.line));
            rightColors.push_back({});
            break;
        case DiffOpKind::Delete:
            leftLines.push_back(QString::fromStdString(op.line));
            leftColors.push_back(kColorRemoved);
            rightLines.push_back("");          // placeholder
            rightColors.push_back(kColorEmpty);
            break;
        case DiffOpKind::Insert:
            leftLines.push_back("");           // placeholder
            leftColors.push_back(kColorEmpty);
            rightLines.push_back(QString::fromStdString(op.line));
            rightColors.push_back(kColorAdded);
            break;
        }
    }
}

// ── applyDiff ─────────────────────────────────────────────────────────────────

void DiffPanel::applyDiff(const DiffResult& diff) {
    currentDiff_ = diff;
    hunkLines_.clear();

    std::vector<QString> leftLines, rightLines;
    std::vector<QColor>  leftColors, rightColors;
    buildLineViews(diff, leftLines, leftColors, rightLines, rightColors);

    // Build plain text
    QString leftText, rightText;
    for (const auto& l : leftLines)  leftText  += l + "\n";
    for (const auto& r : rightLines) rightText += r + "\n";

    leftPane_->setContent(leftText, leftColors);
    rightPane_->setContent(rightText, rightColors);

    // Collect hunk start lines (first inserted/deleted line in each run)
    bool inHunk = false;
    for (int i = 0; i < static_cast<int>(diff.ops.size()); ++i) {
        if (diff.ops[i].kind != DiffOpKind::Equal) {
            if (!inHunk) { hunkLines_.push_back(i); inHunk = true; }
        } else {
            inHunk = false;
        }
    }

    currentHunk_ = 0;
    updateStats();
    emit diffChanged(diff);
}

void DiffPanel::updateStats() {
    if (currentDiff_.isEmpty()) {
        statsLabel_->setText("No diff");
        return;
    }
    int sim = static_cast<int>(currentDiff_.similarity * 100.0);
    statsLabel_->setText(
        QString("<span style='color:#a6e3a1'>+%1</span>  "
                "<span style='color:#f38ba8'>-%2</span>  "
                "<span style='color:#888'>%3% similar</span>")
        .arg(currentDiff_.linesAdded)
        .arg(currentDiff_.linesRemoved)
        .arg(sim));
}

// ── Navigation ────────────────────────────────────────────────────────────────

void DiffPanel::onNextDiff() {
    if (hunkLines_.empty()) return;
    currentHunk_ = (currentHunk_ + 1) % static_cast<int>(hunkLines_.size());
    int line = hunkLines_[currentHunk_];
    leftPane_->scrollToLine(line);
    rightPane_->scrollToLine(line);
}

void DiffPanel::onPrevDiff() {
    if (hunkLines_.empty()) return;
    currentHunk_ = (currentHunk_ - 1 + static_cast<int>(hunkLines_.size())) %
                   static_cast<int>(hunkLines_.size());
    int line = hunkLines_[currentHunk_];
    leftPane_->scrollToLine(line);
    rightPane_->scrollToLine(line);
}

// ── Scroll sync ───────────────────────────────────────────────────────────────

void DiffPanel::onLeftScrollChanged(int value) {
    if (syncingScroll_) return;
    syncingScroll_ = true;
    rightPane_->verticalScrollBar()->setValue(value);
    syncingScroll_ = false;
}

void DiffPanel::onRightScrollChanged(int value) {
    if (syncingScroll_) return;
    syncingScroll_ = true;
    leftPane_->verticalScrollBar()->setValue(value);
    syncingScroll_ = false;
}

// ── Stage selector ────────────────────────────────────────────────────────────

void DiffPanel::onStageSelected(int index) {
    if (index <= 0 || index > static_cast<int>(stages_.size())) return;
    const auto& s = stages_[index - 1];
    auto diff = MyersDiff::diffText(s.before.toStdString(), s.after.toStdString());
    leftLabel_->setText("Before: " + s.name);
    rightLabel_->setText("After: " + s.name);
    applyDiff(diff);
}

// ── Export ────────────────────────────────────────────────────────────────────

void DiffPanel::onExportUnified() {
    QString path = QFileDialog::getSaveFileName(
        this, "Export Diff", "diff.patch", "Patch files (*.patch *.diff)");
    if (path.isEmpty()) return;
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Text)) return;
    f.write(QByteArray::fromStdString(
        currentDiff_.toUnifiedDiff("before", "after")));
}

void DiffPanel::onExportHtml() {
    QString path = QFileDialog::getSaveFileName(
        this, "Export Diff HTML", "diff.html", "HTML files (*.html)");
    if (path.isEmpty()) return;
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Text)) return;
    f.write(QByteArray::fromStdString(currentDiff_.toHtml()));
}

void DiffPanel::onCopyDiff() {
    QApplication::clipboard()->setText(
        QString::fromStdString(
            currentDiff_.toUnifiedDiff("before", "after")));
}

// ── Public API ────────────────────────────────────────────────────────────────

void DiffPanel::setDiff(const QString& before, const QString& after,
                         const QString& stageName) {
    auto diff = MyersDiff::diffText(before.toStdString(), after.toStdString());
    leftLabel_->setText(stageName.isEmpty() ? "Before" : "Before: " + stageName);
    rightLabel_->setText(stageName.isEmpty() ? "After"  : "After: " + stageName);
    applyDiff(diff);
}

void DiffPanel::addStage(const QString& name,
                          const QString& before, const QString& after) {
    stages_.push_back({name, before, after});
    if (stageCombo_)
        stageCombo_->addItem(name);
    // Compute and show the diff immediately so consumers (and tests) can
    // observe currentDiff() right after addStage(). Previously this only
    // populated the dropdown which made addStage feel inert.
    setDiff(before, after, name);
}

void DiffPanel::clear() {
    leftPane_->clear();
    rightPane_->clear();
    currentDiff_ = {};
    hunkLines_.clear();
    statsLabel_->setText("No diff");
    stages_.clear();
    stageCombo_->clear();
    stageCombo_->addItem("<manual>");
}

} // namespace retdec::gui::panels
