/**
 * @file src/gui/panels/tri_pane_code_view.cpp
 * @brief TriPaneCodeView — synchronized three-column code browser implementation.
 *
 * Three SyncedCodePane widgets (assembly | SSA IR | decompiled C/C++) sit
 * inside a QSplitter.  A shared LineMapping table drives cross-pane
 * highlighting whenever the user clicks any line.  Vertical scrolling is
 * kept proportionally in sync across all three panes.
 */

#include "retdec/gui/panels/tri_pane_code_view.h"

#include <QApplication>
#include <QComboBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPainter>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QRegularExpression>
#include <QScrollBar>
#include <QSignalBlocker>
#include <QTimer>
#include <QSplitter>
#include <QTextBlock>
#include <QTextCursor>
#include <QTextDocument>
#include <QVBoxLayout>

namespace retdec::gui::panels {

// ─── Catppuccin Mocha palette ─────────────────────────────────────────────────

namespace {
const QColor kBase      {0x1e, 0x1e, 0x2e};
const QColor kMantle    {0x18, 0x18, 0x25};
const QColor kSurface0  {0x31, 0x32, 0x44};
const QColor kSurface1  {0x45, 0x47, 0x5a};
const QColor kOverlay0  {0x6c, 0x70, 0x86};
const QColor kText      {0xcd, 0xd6, 0xf4};
const QColor kBlue      {0x89, 0xb4, 0xfa};
const QColor kGreen     {0xa6, 0xe3, 0xa1};
const QColor kRed       {0xf3, 0x8b, 0xa8};
const QColor kYellow    {0xf9, 0xe2, 0xaf};
const QColor kPeach     {0xfa, 0xb3, 0x87};
const QColor kMauve     {0xcb, 0xa6, 0xf7};
const QColor kTeal      {0x94, 0xe2, 0xd5};

QTextCharFormat fmt(const QColor& c, bool bold = false) {
    QTextCharFormat f;
    f.setForeground(c);
    if (bold) f.setFontWeight(QFont::Bold);
    return f;
}
} // anonymous namespace

// ─── LineMapping ──────────────────────────────────────────────────────────────

void LineMapping::addEntry(int asmLine, int irLine, int cLine) {
    int idx = entries_.size();
    entries_.append({asmLine, irLine, cLine});
    if (asmLine >= 0) asmIndex_[asmLine] = idx;
    if (irLine  >= 0) irIndex_[irLine]   = idx;
    if (cLine   >= 0) cIndex_[cLine]     = idx;
}

void LineMapping::clear() {
    entries_.clear();
    asmIndex_.clear();
    irIndex_.clear();
    cIndex_.clear();
}

const LineMappingEntry* LineMapping::byAsmLine(int line) const {
    auto it = asmIndex_.find(line);
    if (it == asmIndex_.end()) return nullptr;
    return &entries_[it.value()];
}

const LineMappingEntry* LineMapping::byIrLine(int line) const {
    auto it = irIndex_.find(line);
    if (it == irIndex_.end()) return nullptr;
    return &entries_[it.value()];
}

const LineMappingEntry* LineMapping::byCLine(int line) const {
    auto it = cIndex_.find(line);
    if (it == cIndex_.end()) return nullptr;
    return &entries_[it.value()];
}

// ─── CodeSyntaxHighlighter ────────────────────────────────────────────────────

CodeSyntaxHighlighter::CodeSyntaxHighlighter(QTextDocument* parent, PaneLang lang)
    : QSyntaxHighlighter(parent), lang_(lang) {
    buildRules();
}

void CodeSyntaxHighlighter::setLang(PaneLang lang) {
    lang_ = lang;
    buildRules();
    // Clear per-block userState so stale multi-line comment continuation (C/C++/Java)
    // does not carry into Python/Lua/etc. after buildRules() cleared commentEnd_.
    QTextDocument* doc = document();
    if (doc) {
        for (QTextBlock b = doc->begin(); b.isValid(); b = b.next())
            b.setUserState(-1);
    }
    // Defer rehighlight: synchronous rehighlight() from setLang() combined with
    // SyncedCodePaneTest::processEvents in TearDown could stall indefinitely when
    // the editor has no real viewport size (0×0 in off-screen tests).
    QTimer::singleShot(0, this, [this]() {
        if (document())
            rehighlight();
    });
}

void CodeSyntaxHighlighter::buildRules() {
    rules_.clear();
    commentStart_ = QRegularExpression{};
    commentEnd_   = QRegularExpression{};

    switch (lang_) {

    // ── Assembly ──────────────────────────────────────────────────────────────
    case PaneLang::Asm: {
        // Hex addresses (0x...)
        rules_.append(Rule{QRegularExpression(R"(\b0x[0-9a-fA-F]+\b)"),  fmt(kOverlay0)});
        // Decimal immediates
        rules_.append(Rule{QRegularExpression(R"(\b\d+\b)"),              fmt(kPeach)});
        // Registers (rax, rbx, eax, rcx, rdi, rsi, rsp, rbp, xmm*, ymm*, zmm*, st*, r8-r15)
        rules_.append(Rule{QRegularExpression(
            R"(\b(r[0-9a-f]{2}|r[0-9]+[bwd]?|e[a-z]{2}|[a-d][xhl]|[sb]pl?|[sd]il?|)"
            R"(xmm[0-9]+|ymm[0-9]+|zmm[0-9]+|st[0-7]?|cs|ds|es|fs|gs|ss)\b)"),
            fmt(kMauve)});
        // Mnemonics (first word on a line, after optional address)
        rules_.append(Rule{QRegularExpression(R"((?:^|\s)([a-zA-Z][a-zA-Z0-9]*)(?=\s|$))"),
            fmt(kBlue, true)});
        // Comments (; or //)
        rules_.append(Rule{QRegularExpression(R"((;|//).*)"),             fmt(kGreen)});
        // Labels (word:)
        rules_.append(Rule{QRegularExpression(R"(^\s*[\w.@?$]+:)"),       fmt(kYellow)});
        break;
    }

    // ── SSA IR ────────────────────────────────────────────────────────────────
    case PaneLang::IR: {
        // BB labels (%entry:, %bb1:)
        rules_.append(Rule{QRegularExpression(R"(^\s*%\w+:)"),            fmt(kPeach, true)});
        // SSA names (%a, %x.1)
        rules_.append(Rule{QRegularExpression(R"(%\w+)"),                 fmt(kMauve)});
        // Keywords (delimiter rx — pattern contains )" which would end R"(...)" )
        rules_.append(Rule{
            QRegularExpression(
                R"rx(\b(define|declare|call|ret|br|load|store|alloca|getelementptr|icmp|fcmp|phi|select|switch|unreachable|add|sub|mul|div|and|or|xor|shl|lshr|ashr|trunc|zext|sext|bitcast|inttoptr|ptrtoint|i1|i8|i16|i32|i64|float|double|void|ptr)\b)rx"),
            fmt(kBlue)});
        // Types with bit widths (i32, i64)
        rules_.append(Rule{QRegularExpression(R"(\bi\d+\b)"),             fmt(kYellow)});
        // Integer literals
        rules_.append(Rule{QRegularExpression(R"(\b-?\d+\b)"),            fmt(kPeach)});
        // Comments (//)
        rules_.append(Rule{QRegularExpression(R"(//.*)"),                 fmt(kOverlay0)});
        break;
    }

    // ── C ─────────────────────────────────────────────────────────────────────
    case PaneLang::C:
    case PaneLang::Cxx: {
        // Preprocessor (#include, #define, #ifdef, …)
        rules_.append(Rule{QRegularExpression(R"(#\s*\w+[^\n]*)"),        fmt(kRed)});
        // String literals
        rules_.append(Rule{QRegularExpression(R"("(?:[^"\\]|\\.)*")"),    fmt(kGreen)});
        // Character literals
        rules_.append(Rule{QRegularExpression(R"('(?:[^'\\]|\\.)*')"),    fmt(kGreen)});
        // C/C++ keywords
        const QString kwRe = lang_ == PaneLang::Cxx
            ? R"(\b(auto|break|case|catch|class|const|constexpr|continue|default|)"
              R"(delete|do|double|else|enum|explicit|extern|false|float|for|friend|)"
              R"(goto|if|inline|int|long|mutable|namespace|new|nullptr|operator|)"
              R"(private|protected|public|register|return|short|signed|sizeof|static|)"
              R"(struct|switch|template|this|throw|true|try|typedef|typeid|typename|)"
              R"(union|unsigned|using|virtual|void|volatile|while)\b)"
            : R"(\b(auto|break|case|const|continue|default|do|double|else|enum|)"
              R"(extern|float|for|goto|if|inline|int|long|register|return|short|)"
              R"(signed|sizeof|static|struct|switch|typedef|union|unsigned|void|)"
              R"(volatile|while)\b)";
        rules_.append(Rule{QRegularExpression(kwRe), fmt(kBlue, true)});
        // Type keywords
        rules_.append(Rule{QRegularExpression(
            R"(\b(uint8_t|uint16_t|uint32_t|uint64_t|int8_t|int16_t|int32_t|int64_t|)"
            R"(size_t|ptrdiff_t|uintptr_t|intptr_t|bool|wchar_t|char)\b)"),
            fmt(kYellow)});
        // Numeric literals
        rules_.append(Rule{QRegularExpression(R"(\b(0x[0-9a-fA-F]+|\d+[uUlL]*)\b)"), fmt(kPeach)});
        // Line comments (//)
        rules_.append(Rule{QRegularExpression(R"(//.*)"),                 fmt(kOverlay0)});
        // Multi-line comment delimiters
        commentStart_ = QRegularExpression(R"(/\*)");
        commentEnd_   = QRegularExpression(R"(\*/)");
        commentFmt_   = fmt(kOverlay0);
        break;
    }

    // ── C# ────────────────────────────────────────────────────────────────────
    case PaneLang::CSharp: {
        rules_.append(Rule{QRegularExpression(R"(@?"(?:[^"\\]|\\.)*")"),  fmt(kGreen)});
        rules_.append(Rule{QRegularExpression(
            R"(\b(abstract|as|base|bool|break|byte|case|catch|char|checked|class|)"
            R"(const|continue|decimal|default|delegate|do|double|else|enum|event|)"
            R"(explicit|extern|false|finally|fixed|float|for|foreach|goto|if|implicit|)"
            R"(in|int|interface|internal|is|lock|long|namespace|new|null|object|)"
            R"(operator|out|override|params|private|protected|public|readonly|ref|)"
            R"(return|sbyte|sealed|short|sizeof|stackalloc|static|string|struct|)"
            R"(switch|this|throw|true|try|typeof|uint|ulong|unchecked|unsafe|ushort|)"
            R"(using|virtual|void|volatile|while|var|async|await|yield)\b)"),
            fmt(kBlue, true)});
        rules_.append(Rule{QRegularExpression(R"(\b(0x[0-9a-fA-F]+|\d+[uUlLmM]?)\b)"), fmt(kPeach)});
        rules_.append(Rule{QRegularExpression(R"(//.*)"), fmt(kOverlay0)});
        commentStart_ = QRegularExpression(R"(/\*)");
        commentEnd_   = QRegularExpression(R"(\*/)");
        commentFmt_   = fmt(kOverlay0);
        break;
    }

    // ── Java ──────────────────────────────────────────────────────────────────
    case PaneLang::Java: {
        rules_.append(Rule{QRegularExpression(R"("(?:[^"\\]|\\.)*")"),    fmt(kGreen)});
        rules_.append(Rule{QRegularExpression(
            R"(\b(abstract|assert|boolean|break|byte|case|catch|char|class|const|)"
            R"(continue|default|do|double|else|enum|extends|final|finally|float|for|)"
            R"(goto|if|implements|import|instanceof|int|interface|long|native|new|null|)"
            R"(package|private|protected|public|return|short|static|strictfp|super|)"
            R"(switch|synchronized|this|throw|throws|transient|true|false|try|void|)"
            R"(volatile|while|var|sealed|permits|record)\b)"),
            fmt(kBlue, true)});
        rules_.append(Rule{QRegularExpression(R"(\b(0x[0-9a-fA-F]+|\d+[lLfFdD]?)\b)"), fmt(kPeach)});
        rules_.append(Rule{QRegularExpression(R"(//.*)"), fmt(kOverlay0)});
        commentStart_ = QRegularExpression(R"(/\*)");
        commentEnd_   = QRegularExpression(R"(\*/)");
        commentFmt_   = fmt(kOverlay0);
        break;
    }

    // ── Python ────────────────────────────────────────────────────────────────
    case PaneLang::Python: {
        // Strings: avoid nested quantifiers like (?:[^"\\]|\\.)* that can trigger
        // catastrophic backtracking on long unterminated quotes in the editor.
        rules_.append(Rule{QRegularExpression(R"("([^"\\]|\\.)*")"), fmt(kGreen)});
        rules_.append(Rule{QRegularExpression(R"('([^'\\]|\\.)*')"), fmt(kGreen)});
        rules_.append(Rule{QRegularExpression(
            R"(\b(False|None|True|and|as|assert|async|await|break|class|continue|)"
            R"(def|del|elif|else|except|finally|for|from|global|if|import|in|is|lambda|)"
            R"(nonlocal|not|or|pass|raise|return|try|while|with|yield)\b)"),
            fmt(kBlue, true)});
        rules_.append(Rule{QRegularExpression(
            R"(\b(int|str|float|bytes|bool|list|dict|set|tuple|type|object|complex|)"
            R"(bytearray|memoryview|range|slice|frozenset|property|classmethod|staticmethod|)"
            R"(super|print|len|zip|map|filter|enumerate|sorted|reversed|any|all|min|max|)"
            R"(abs|round|open|input|iter|next|repr|isinstance|issubclass|hasattr|getattr|)"
            R"(setattr|delattr|vars|dir|id|hash|callable|chr|ord|hex|bin|oct)\b)"),
            fmt(kTeal)});
        rules_.append(Rule{QRegularExpression(R"(\b(0x[0-9a-fA-F]+|\d+[jJ]?)\b)"), fmt(kPeach)});
        rules_.append(Rule{QRegularExpression(R"(#.*)"), fmt(kOverlay0)});
        break;
    }

    // ── Lua ───────────────────────────────────────────────────────────────────
    case PaneLang::Lua: {
        rules_.append(Rule{QRegularExpression(R"("(?:[^"\\]|\\.)*"|'(?:[^'\\]|\\.)*')"), fmt(kGreen)});
        rules_.append(Rule{QRegularExpression(
            R"(\b(and|break|do|else|elseif|end|false|for|function|goto|if|in|local|)"
            R"(nil|not|or|repeat|return|then|true|until|while)\b)"),
            fmt(kBlue, true)});
        rules_.append(Rule{QRegularExpression(R"(\b\d+(\.\d*)?\b)"),      fmt(kPeach)});
        rules_.append(Rule{QRegularExpression(R"(--.*)"),                 fmt(kOverlay0)});
        break;
    }
    }
}

void CodeSyntaxHighlighter::highlightBlock(const QString& text) {
    // Multi-line comment support
    const bool hasMultiLine = commentStart_.isValid() && commentEnd_.isValid();
    int startIndex = 0;
    if (previousBlockState() == 1) {
        if (!hasMultiLine) {
            // e.g. switched from C/C++ (/* */) to Python — old block state must not run
            // commentEnd_.match() with cleared patterns (invalid / empty regex).
            setCurrentBlockState(0);
        } else {
            // We are inside a block comment from a prior block.
            auto m = commentEnd_.match(text);
            int endIndex   = m.hasMatch() ? m.capturedStart() + m.capturedLength() : text.length();
            setFormat(0, endIndex, commentFmt_);
            setCurrentBlockState(m.hasMatch() ? 0 : 1);
            startIndex = endIndex;
        }
    } else {
        setCurrentBlockState(0);
    }

    // Apply single-line rules (only outside block comments).
    for (const auto& rule : rules_) {
        auto it = rule.pattern.globalMatch(text);
        int guard = 0;
        constexpr int kMaxMatchesPerRule = 4096;
        while (it.hasNext()) {
            auto m = it.next();
            // Zero-length matches: must break — "continue" can loop forever if the
            // iterator keeps yielding the same empty match (seen in SetLang / Python).
            if (m.capturedLength() <= 0)
                break;
            setFormat(m.capturedStart(), m.capturedLength(), rule.format);
            if (++guard >= kMaxMatchesPerRule)
                break;
        }
    }

    // Handle opening block comments
    if (hasMultiLine && currentBlockState() != 1) {
        auto m = commentStart_.match(text, startIndex);
        while (m.hasMatch()) {
            auto endM  = commentEnd_.match(text, m.capturedStart() + m.capturedLength());
            int endIdx = endM.hasMatch()
                         ? endM.capturedStart() + endM.capturedLength()
                         : text.length();
            setFormat(m.capturedStart(), endIdx - m.capturedStart(), commentFmt_);
            if (!endM.hasMatch()) { setCurrentBlockState(1); break; }
            m = commentStart_.match(text, endIdx);
        }
    }
}

// ─── LineNumberArea ───────────────────────────────────────────────────────────

LineNumberArea::LineNumberArea(SyncedCodePane* pane)
    : QWidget(pane), pane_(pane) {
    setAutoFillBackground(true);
    QPalette pal;
    pal.setColor(QPalette::Window, kMantle);
    setPalette(pal);
}

QSize LineNumberArea::sizeHint() const {
    return {pane_->lineNumberAreaWidth(), 0};
}

void LineNumberArea::paintEvent(QPaintEvent* event) {
    pane_->lineNumberAreaPaintEvent(event);
}

// ─── SyncedCodePane ───────────────────────────────────────────────────────────

SyncedCodePane::SyncedCodePane(PaneLang lang, const QString& title,
                                QWidget* parent)
    : QWidget(parent), title_(title), lang_(lang) {
    setupUI();
}

void SyncedCodePane::setupUI() {
    titleLabel_ = new QLabel(title_, this);
    titleLabel_->setAlignment(Qt::AlignCenter);
    titleLabel_->setStyleSheet(
        "color: #6c7086; font-size: 10px; font-weight: bold; "
        "background: #181825; padding: 2px;");

    editor_ = new CodeEditor(this);
    editor_->setReadOnly(true);
    editor_->setLineWrapMode(QPlainTextEdit::NoWrap);
    editor_->setPlaceholderText("No content loaded…");

    // Catppuccin Mocha colours for the editor
    QPalette pal = editor_->palette();
    pal.setColor(QPalette::Base,  kBase);
    pal.setColor(QPalette::Text,  kText);
    editor_->setPalette(pal);

    QFont mono("Cascadia Code", 10);
    mono.setStyleHint(QFont::Monospace);
    mono.setFixedPitch(true);
    editor_->setFont(mono);

    lineNumArea_ = new LineNumberArea(this);
    hl_          = new CodeSyntaxHighlighter(editor_->document(), lang_);

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    layout->addWidget(titleLabel_);

    // Overlay the line number area on the left of the editor using a nested layout.
    auto* editorRow = new QWidget(this);
    auto* hl        = new QHBoxLayout(editorRow);
    hl->setContentsMargins(0, 0, 0, 0);
    hl->setSpacing(0);
    hl->addWidget(lineNumArea_);
    hl->addWidget(editor_);
    layout->addWidget(editorRow);

    connect(editor_, &QPlainTextEdit::blockCountChanged,
            this,    &SyncedCodePane::updateLineNumberAreaWidth);
    connect(editor_, &QPlainTextEdit::updateRequest,
            this,    &SyncedCodePane::updateLineNumberArea);
    connect(editor_, &QPlainTextEdit::cursorPositionChanged,
            this,    &SyncedCodePane::onCursorPositionChanged);
    connect(editor_->verticalScrollBar(), &QScrollBar::valueChanged,
            this,    &SyncedCodePane::onScrollValueChanged);

    updateLineNumberAreaWidth(0);
}

void SyncedCodePane::setContent(const QString& text) {
    // Block QTextDocument signals during setPlainText so QSyntaxHighlighter
    // does not re-enter layout/highlight for every incremental change (can
    // appear as a hang in tests and when loading large listings).
    {
        QSignalBlocker docBlock(editor_->document());
        editor_->setPlainText(text);
    }
    clearHighlight();
    // Defer rehighlight (same as setLang): synchronous rehighlight from setContent
    // stacks with QTextLayout/updateRequest and can stall TriPane/loadFunction in tests.
    QTimer::singleShot(0, this, [this]() {
        if (hl_ && hl_->document())
            hl_->rehighlight();
    });
}

void SyncedCodePane::setLang(PaneLang lang) {
    lang_ = lang;
    hl_->setLang(lang);
}

int SyncedCodePane::lineCount() const {
    return editor_->document()->blockCount();
}

void SyncedCodePane::highlightLine(int line) {
    highlightedLine_ = line;
    if (line < 1) { clearHighlight(); return; }

    QList<QTextEdit::ExtraSelection> sels;
    QTextEdit::ExtraSelection sel;
    sel.format.setBackground(QColor(0x45, 0x47, 0x5a, 160));
    sel.format.setProperty(QTextFormat::FullWidthSelection, true);
    sel.cursor = editor_->textCursor();
    sel.cursor.movePosition(QTextCursor::Start);
    sel.cursor.movePosition(QTextCursor::NextBlock, QTextCursor::MoveAnchor,
                            line - 1);
    sel.cursor.clearSelection();
    sels.append(sel);
    editor_->setExtraSelections(sels);
}

void SyncedCodePane::scrollToLine(int line) {
    if (line < 1) return;
    QTextCursor c(editor_->document()->findBlockByLineNumber(line - 1));
    suppressScrollSignal_ = true;
    editor_->setTextCursor(c);
    editor_->centerCursor();
    suppressScrollSignal_ = false;
}

void SyncedCodePane::clearHighlight() {
    highlightedLine_ = -1;
    editor_->setExtraSelections({});
}

int SyncedCodePane::lineNumberAreaWidth() const {
    int digits = 1;
    int max    = qMax(1, editor_->document()->blockCount());
    while (max >= 10) { max /= 10; ++digits; }
    return 4 + editor_->fontMetrics().horizontalAdvance('9') * (digits + 1);
}

void SyncedCodePane::lineNumberAreaPaintEvent(QPaintEvent* event) {
    QPainter painter(lineNumArea_);
    painter.fillRect(event->rect(), kMantle);
    QTextBlock block = editor_->firstVisibleBlockPub();
    int blockNumber  = block.blockNumber();
    int top    = qRound(editor_->blockBoundingGeometryPub(block)
                        .translated(editor_->contentOffsetPub()).top());
    int bottom = top + qRound(editor_->blockBoundingRectPub(block).height());
    QFont f = editor_->font();
    f.setPointSize(f.pointSize() - 1);
    painter.setFont(f);

    while (block.isValid() && top <= event->rect().bottom()) {
        if (block.isVisible() && bottom >= event->rect().top()) {
            bool highlighted = (blockNumber + 1) == highlightedLine_;
            painter.setPen(highlighted ? kBlue : kOverlay0);
            painter.drawText(0, top, lineNumArea_->width() - 4,
                             editor_->fontMetrics().height(),
                             Qt::AlignRight,
                             QString::number(blockNumber + 1));
        }
        block  = block.next();
        top    = bottom;
        bottom = top + qRound(editor_->blockBoundingRectPub(block).height());
        ++blockNumber;
    }
}

void SyncedCodePane::onCursorPositionChanged() {
    QTextCursor c = editor_->textCursor();
    int line = c.blockNumber() + 1;
    emit lineActivated(line);
}

void SyncedCodePane::onScrollValueChanged(int value) {
    lineNumArea_->update();
    if (!suppressScrollSignal_)
        emit verticalScrollChanged(value);
}

void SyncedCodePane::updateLineNumberAreaWidth(int /*newBlockCount*/) {
    editor_->setViewportMarginsPub(lineNumberAreaWidth(), 0, 0, 0);
    lineNumArea_->setFixedWidth(lineNumberAreaWidth());
}

void SyncedCodePane::updateLineNumberArea(const QRect& rect, int dy) {
    if (lineNumberAreaUpdateGuard_)
        return;
    lineNumberAreaUpdateGuard_ = true;
    if (dy)  lineNumArea_->scroll(0, dy);
    else     lineNumArea_->update(0, rect.y(),
                                   lineNumArea_->width(), rect.height());
    // In unit tests the QPlainTextEdit often has a 0×0 viewport; QRect::contains treats
    // that degenerate rect as contained by almost any update rect, so we would call
    // updateLineNumberAreaWidth on every paint slice and flood the event loop.
    const QRect vp = editor_->viewport()->rect();
    if (vp.width() > 0 && vp.height() > 0 && rect.contains(vp))
        updateLineNumberAreaWidth(0);
    lineNumberAreaUpdateGuard_ = false;
}

// ─── TriPaneCodeView ──────────────────────────────────────────────────────────

TriPaneCodeView::TriPaneCodeView(QWidget* parent)
    : PanelBase("Tri-Pane View", parent) {
    setupUI();
}

// ── UI construction ───────────────────────────────────────────────────────────

void TriPaneCodeView::setupUI() {
    asmPane_ = new SyncedCodePane(PaneLang::Asm,  "Assembly",     this);
    irPane_  = new SyncedCodePane(PaneLang::IR,   "SSA IR",       this);
    cPane_   = new SyncedCodePane(PaneLang::C,    "Decompiled C", this);

    splitter_ = new QSplitter(Qt::Horizontal, this);
    splitter_->addWidget(asmPane_);
    splitter_->addWidget(irPane_);
    splitter_->addWidget(cPane_);
    splitter_->setStretchFactor(0, 1);
    splitter_->setStretchFactor(1, 1);
    splitter_->setStretchFactor(2, 1);
    splitter_->setHandleWidth(3);

    // Find bar
    findBar_ = new QWidget(this);
    auto* fbLayout = new QHBoxLayout(findBar_);
    fbLayout->setContentsMargins(4, 2, 4, 2);
    auto* findIcon = new QLabel("Find:", findBar_);
    findIcon->setStyleSheet("color: #6c7086;");
    findEdit_ = new QLineEdit(findBar_);
    findEdit_->setPlaceholderText("Search all panes…");
    findEdit_->setClearButtonEnabled(true);
    auto* findNextBtn = new QPushButton("▼", findBar_);
    findNextBtn->setFixedWidth(24);
    auto* findPrevBtn = new QPushButton("▲", findBar_);
    findPrevBtn->setFixedWidth(24);
    findClose_ = new QPushButton("✕", findBar_);
    findClose_->setFixedWidth(24);
    fbLayout->addWidget(findIcon);
    fbLayout->addWidget(findEdit_);
    fbLayout->addWidget(findPrevBtn);
    fbLayout->addWidget(findNextBtn);
    fbLayout->addWidget(findClose_);
    findBar_->hide();

    connect(findEdit_,    &QLineEdit::textChanged,
            this,         &TriPaneCodeView::onFindTextChanged);
    connect(findNextBtn,  &QPushButton::clicked,
            this,         &TriPaneCodeView::onFindNext);
    connect(findPrevBtn,  &QPushButton::clicked,
            this,         &TriPaneCodeView::onFindPrev);
    connect(findClose_,   &QPushButton::clicked,
            this,         &TriPaneCodeView::hideFindBar);

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    layout->addWidget(buildToolbar());
    layout->addWidget(splitter_, 1);
    layout->addWidget(findBar_);

    // Cross-pane line activation
    connect(asmPane_, &SyncedCodePane::lineActivated,
            this,     &TriPaneCodeView::onAsmLineActivated);
    connect(irPane_,  &SyncedCodePane::lineActivated,
            this,     &TriPaneCodeView::onIrLineActivated);
    connect(cPane_,   &SyncedCodePane::lineActivated,
            this,     &TriPaneCodeView::onCLineActivated);

    // Synchronized scrolling
    connect(asmPane_, &SyncedCodePane::verticalScrollChanged,
            this,     &TriPaneCodeView::onAsmScrollChanged);
    connect(irPane_,  &SyncedCodePane::verticalScrollChanged,
            this,     &TriPaneCodeView::onIrScrollChanged);
    connect(cPane_,   &SyncedCodePane::verticalScrollChanged,
            this,     &TriPaneCodeView::onCScrollChanged);
}

QWidget* TriPaneCodeView::buildToolbar() {
    auto* tb = new QWidget(this);
    tb->setAutoFillBackground(true);
    QPalette pal;
    pal.setColor(QPalette::Window, kMantle);
    tb->setPalette(pal);

    auto* layout = new QHBoxLayout(tb);
    layout->setContentsMargins(4, 2, 4, 2);
    layout->setSpacing(4);

    backBtn_ = new QPushButton("◀", tb);
    backBtn_->setFixedWidth(26);
    backBtn_->setToolTip("Navigate back (Alt+←)");
    backBtn_->setEnabled(false);

    fwdBtn_ = new QPushButton("▶", tb);
    fwdBtn_->setFixedWidth(26);
    fwdBtn_->setToolTip("Navigate forward (Alt+→)");
    fwdBtn_->setEnabled(false);

    funcLabel_ = new QLabel("No function loaded", tb);
    funcLabel_->setStyleSheet("color: #cdd6f4; font-weight: bold;");

    langCombo_ = new QComboBox(tb);
    langCombo_->addItems({"C", "C++", "C#", "Java", "Python", "Lua"});
    langCombo_->setToolTip("Output language");
    langCombo_->setFixedWidth(72);

    auto* findBtn = new QPushButton("⌕", tb);
    findBtn->setFixedWidth(26);
    findBtn->setToolTip("Find in panes (Ctrl+F)");
    findBtn->setShortcut(QKeySequence("Ctrl+F"));

    layout->addWidget(backBtn_);
    layout->addWidget(fwdBtn_);
    layout->addWidget(funcLabel_, 1);
    layout->addWidget(new QLabel("Lang:", tb));
    layout->addWidget(langCombo_);
    layout->addWidget(findBtn);

    connect(backBtn_,  &QPushButton::clicked,
            this,      &TriPaneCodeView::onBackClicked);
    connect(fwdBtn_,   &QPushButton::clicked,
            this,      &TriPaneCodeView::onForwardClicked);
    connect(langCombo_, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this,      &TriPaneCodeView::onLangChanged);
    connect(findBtn,   &QPushButton::clicked,
            this,      &TriPaneCodeView::showFindBar);

    return tb;
}

// ── Content loading ───────────────────────────────────────────────────────────

void TriPaneCodeView::loadFunction(uint64_t address,
                                    const QString& asmText,
                                    const QString& irText,
                                    const QString& cText) {
    // Block scroll signals while all three panes update; otherwise valueChanged
    // can cross-trigger proportional sync in a deep stack until stack overflow
    // or apparent hang (especially under tests / headless).
    QSignalBlocker bAsm(asmPane_->editor()->verticalScrollBar());
    QSignalBlocker bIr(irPane_->editor()->verticalScrollBar());
    QSignalBlocker bC(cPane_->editor()->verticalScrollBar());
    asmPane_->setContent(asmText);
    irPane_->setContent(irText);
    cPane_->setContent(cText);
    funcLabel_->setText(QString("0x%1").arg(address, 0, 16));
}

void TriPaneCodeView::setLineMapping(const LineMapping& mapping) {
    mapping_ = mapping;
}

void TriPaneCodeView::setOutputLanguage(OutputLang lang) {
    outputLang_ = lang;
    cPane_->setLang(outputLangToPaneLang(lang));

    // Update combo box without triggering onLangChanged recursively
    QSignalBlocker blocker(langCombo_);
    langCombo_->setCurrentIndex(static_cast<int>(lang));

    // Update pane title
    const char* titles[] = {"Decompiled C","Decompiled C++","Decompiled C#",
                             "Decompiled Java","Decompiled Python","Decompiled Lua"};
    // Update title label inside cPane_
    if (auto* lbl = cPane_->findChild<QLabel*>()) {
        lbl->setText(titles[static_cast<int>(lang)]);
    }
}

PaneLang TriPaneCodeView::outputLangToPaneLang(OutputLang lang) const {
    switch (lang) {
    case OutputLang::C:       return PaneLang::C;
    case OutputLang::Cxx:     return PaneLang::Cxx;
    case OutputLang::CSharp:  return PaneLang::CSharp;
    case OutputLang::Java:    return PaneLang::Java;
    case OutputLang::Python:  return PaneLang::Python;
    case OutputLang::Lua:     return PaneLang::Lua;
    }
    return PaneLang::C;
}

// ── Navigation ────────────────────────────────────────────────────────────────

void TriPaneCodeView::navigateTo(uint64_t address) {
    emit addressNavigated(address);
}

void TriPaneCodeView::navigateBack() {
    if (navIdx_ > 0) {
        --navIdx_;
        updateNavButtons();
        emit addressNavigated(navHistory_[navIdx_].address);
    }
}

void TriPaneCodeView::navigateForward() {
    if (navIdx_ < navHistory_.size() - 1) {
        ++navIdx_;
        updateNavButtons();
        emit addressNavigated(navHistory_[navIdx_].address);
    }
}

void TriPaneCodeView::updateNavButtons() {
    backBtn_->setEnabled(navIdx_ > 0);
    fwdBtn_->setEnabled(navIdx_ < navHistory_.size() - 1);
}

void TriPaneCodeView::onFunctionSelected(uint64_t address, const QString& name) {
    funcLabel_->setText(QString("%1  @ 0x%2").arg(name).arg(address, 0, 16));

    // Truncate forward history on new navigation
    while (navHistory_.size() > navIdx_ + 1)
        navHistory_.removeLast();
    navHistory_.append({address, name});
    if (navHistory_.size() > kMaxHistory)
        navHistory_.removeFirst();
    navIdx_ = navHistory_.size() - 1;
    updateNavButtons();
}

// ── Cross-pane highlighting ────────────────────────────────────────────────────

void TriPaneCodeView::applyHighlight(int asmLine, int irLine, int cLine) {
    asmPane_->highlightLine(asmLine);
    irPane_->highlightLine(irLine);
    cPane_->highlightLine(cLine);
    if (asmLine > 0) asmPane_->scrollToLine(asmLine);
    if (irLine  > 0) irPane_->scrollToLine(irLine);
    if (cLine   > 0) cPane_->scrollToLine(cLine);
}

void TriPaneCodeView::onAsmLineActivated(int line) {
    if (mapping_.isEmpty()) return;
    const auto* e = mapping_.byAsmLine(line);
    if (e) applyHighlight(e->asmLine, e->irLine, e->cLine);
}

void TriPaneCodeView::onIrLineActivated(int line) {
    if (mapping_.isEmpty()) return;
    const auto* e = mapping_.byIrLine(line);
    if (e) applyHighlight(e->asmLine, e->irLine, e->cLine);
}

void TriPaneCodeView::onCLineActivated(int line) {
    if (mapping_.isEmpty()) return;
    const auto* e = mapping_.byCLine(line);
    if (e) applyHighlight(e->asmLine, e->irLine, e->cLine);
}

// ── Synchronized scrolling ────────────────────────────────────────────────────

// Proportional scroll sync: maps one pane's absolute scroll value to an
// equivalent relative position in another pane.
static int proportionalScroll(int value, int srcMax, int dstMax) {
    if (srcMax <= 0) return 0;
    return static_cast<int>(static_cast<double>(value) / srcMax * dstMax);
}

void TriPaneCodeView::onAsmScrollChanged(int value) {
    if (syncingScroll_) return;
    syncingScroll_ = true;
    auto* irSb  = irPane_->editor()->verticalScrollBar();
    auto* cSb   = cPane_->editor()->verticalScrollBar();
    auto* asmSb = asmPane_->editor()->verticalScrollBar();
    irSb->setValue(proportionalScroll(value, asmSb->maximum(), irSb->maximum()));
    cSb->setValue(proportionalScroll(value,  asmSb->maximum(), cSb->maximum()));
    syncingScroll_ = false;
}

void TriPaneCodeView::onIrScrollChanged(int value) {
    if (syncingScroll_) return;
    syncingScroll_ = true;
    auto* irSb  = irPane_->editor()->verticalScrollBar();
    auto* cSb   = cPane_->editor()->verticalScrollBar();
    auto* asmSb = asmPane_->editor()->verticalScrollBar();
    asmSb->setValue(proportionalScroll(value, irSb->maximum(), asmSb->maximum()));
    cSb->setValue(proportionalScroll(value,   irSb->maximum(), cSb->maximum()));
    syncingScroll_ = false;
}

void TriPaneCodeView::onCScrollChanged(int value) {
    if (syncingScroll_) return;
    syncingScroll_ = true;
    auto* cSb   = cPane_->editor()->verticalScrollBar();
    auto* irSb  = irPane_->editor()->verticalScrollBar();
    auto* asmSb = asmPane_->editor()->verticalScrollBar();
    asmSb->setValue(proportionalScroll(value, cSb->maximum(), asmSb->maximum()));
    irSb->setValue(proportionalScroll(value,  cSb->maximum(), irSb->maximum()));
    syncingScroll_ = false;
}

// ── Find bar ─────────────────────────────────────────────────────────────────

void TriPaneCodeView::showFindBar() {
    findBar_->show();
    findEdit_->setFocus();
    findEdit_->selectAll();
}

void TriPaneCodeView::hideFindBar() {
    findBar_->hide();
    findEdit_->clear();
    asmPane_->clearHighlight();
    irPane_->clearHighlight();
    cPane_->clearHighlight();
}

static void searchInPane(QPlainTextEdit* editor, const QString& text,
                          bool forward = true) {
    if (text.isEmpty()) return;
    QTextDocument::FindFlags flags;
    if (!forward) flags |= QTextDocument::FindBackward;
    if (!editor->find(text, flags)) {
        // Wrap around
        QTextCursor c = editor->textCursor();
        c.movePosition(forward ? QTextCursor::Start : QTextCursor::End);
        editor->setTextCursor(c);
        editor->find(text, flags);
    }
}

void TriPaneCodeView::onFindTextChanged(const QString& text) {
    (void)text;  // Live preview on Enter / buttons only
}

void TriPaneCodeView::onFindNext() {
    const QString text = findEdit_->text();
    searchInPane(asmPane_->editor(), text, true);
    searchInPane(irPane_->editor(),  text, true);
    searchInPane(cPane_->editor(),   text, true);
}

void TriPaneCodeView::onFindPrev() {
    const QString text = findEdit_->text();
    searchInPane(asmPane_->editor(), text, false);
    searchInPane(irPane_->editor(),  text, false);
    searchInPane(cPane_->editor(),   text, false);
}

// ── Toolbar slots ─────────────────────────────────────────────────────────────

void TriPaneCodeView::onBackClicked() {
    navigateBack();
}

void TriPaneCodeView::onForwardClicked() {
    navigateForward();
}

void TriPaneCodeView::onLangChanged(int index) {
    setOutputLanguage(static_cast<OutputLang>(index));
}

// ── Clear ─────────────────────────────────────────────────────────────────────

void TriPaneCodeView::clear() {
    asmPane_->setContent({});
    irPane_->setContent({});
    cPane_->setContent({});
    mapping_.clear();
    navHistory_.clear();
    navIdx_ = -1;
    updateNavButtons();
    funcLabel_->setText("No function loaded");
    hideFindBar();
}

} // namespace retdec::gui::panels
