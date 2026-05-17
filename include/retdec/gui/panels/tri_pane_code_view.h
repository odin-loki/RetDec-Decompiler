/**
 * @file include/retdec/gui/panels/tri_pane_code_view.h
 * @brief TriPaneCodeView — synchronized three-column code browser.
 *
 * ## Layout
 *
 * ┌─────────────────────────────────────────────────────────────────────┐
 * │  [◀][▶]  func_name @ 0x401000  │ Find: [_____________] [✕]  [⊕][⊗] │
 * ├──────────────────┬──────────────────┬──────────────────────────────┤
 * │   ASSEMBLY       │   SSA IR         │   DECOMPILED C / C++          │
 * │                  │                  │                               │
 * │  1│ 401000 push  │  1│ %entry:      │  1│ int sub_401000(…) {       │
 * │  2│ 401002 mov   │  2│   %a = …     │  2│   int a = …;              │
 * │  3│ …            │  3│   …          │  3│   …                       │
 * └──────────────────┴──────────────────┴──────────────────────────────┘
 *
 * ## Cross-pane Sync
 *
 * A `LineMapping` table records (asm_line ↔ ir_line ↔ c_line) triples.
 * Clicking any line highlights the corresponding lines in the other two
 * panes and scrolls them into view.
 *
 * ## Syntax Highlighting
 *
 * `CodeSyntaxHighlighter` applies per-pane token colouring:
 *   - Asm:  mnemonics (blue), registers (mauve), immediates (peach),
 *           addresses (overlay0), comments (green)
 *   - IR:   keywords (blue), SSA names (mauve), types (yellow),
 *           BB labels (peach)
 *   - C/C++: keywords (blue), types (yellow), strings (green),
 *             comments (overlay0), macros (red)
 *
 * ## Navigation History
 *
 * Back/forward buttons remember the last 32 (function, address) pairs.
 *
 * ## API
 *
 *   view->loadFunction(address, asmText, irText, cText);
 *   view->setLineMapping(mapping);
 *   view->setOutputLanguage(OutputLang::Cxx);
 *   view->navigateTo(address);
 */

#ifndef RETDEC_GUI_PANELS_TRI_PANE_CODE_VIEW_H
#define RETDEC_GUI_PANELS_TRI_PANE_CODE_VIEW_H

#include "retdec/gui/panels/panel_base.h"

#include <QList>
#include <QMap>
#include <QRegularExpression>
#include <QSyntaxHighlighter>
#include <QVector>

#include <QPlainTextEdit>

QT_BEGIN_NAMESPACE
class QLabel;
class QLineEdit;
class QSplitter;
class QPushButton;
class QComboBox;
class QScrollBar;
class QTextCharFormat;
QT_END_NAMESPACE

// Thin subclass to expose protected QPlainTextEdit helpers needed by the
// line-number area widget.
class CodeEditor : public QPlainTextEdit {
    Q_OBJECT
public:
    explicit CodeEditor(QWidget* parent = nullptr) : QPlainTextEdit(parent) {}

    QTextBlock  firstVisibleBlockPub() const         { return firstVisibleBlock(); }
    QRectF      blockBoundingGeometryPub(const QTextBlock& b) const
                                                     { return blockBoundingGeometry(b); }
    QPointF     contentOffsetPub() const             { return contentOffset(); }
    QRectF      blockBoundingRectPub(const QTextBlock& b) const
                                                     { return blockBoundingRect(b); }
    void        setViewportMarginsPub(int l, int t, int r, int b)
                                                     { setViewportMargins(l, t, r, b); }
};

namespace retdec::gui::panels {

// ─── OutputLang ──────────────────────────────────────────────────────────────

enum class OutputLang { C, Cxx, CSharp, Java, Python, Lua };

// ─── LineMapping ─────────────────────────────────────────────────────────────

/**
 * @brief Bidirectional mapping between assembly, IR, and source lines.
 *
 * A triple (asmLine, irLine, cLine) associates one assembly instruction with
 * its SSA IR statement and the corresponding source-code line (1-based).
 * Any index may be -1 (no correspondence for that pane).
 */
struct LineMappingEntry {
    int asmLine = -1;  ///< 1-based line in the assembly pane (-1 = none)
    int irLine  = -1;  ///< 1-based line in the IR pane (-1 = none)
    int cLine   = -1;  ///< 1-based line in the C/source pane (-1 = none)
};

class LineMapping {
public:
    void addEntry(int asmLine, int irLine, int cLine);
    void clear();
    bool isEmpty() const { return entries_.isEmpty(); }

    /// Returns the entry whose asmLine == line, or nullptr.
    const LineMappingEntry* byAsmLine(int line) const;
    /// Returns the entry whose irLine == line, or nullptr.
    const LineMappingEntry* byIrLine(int line) const;
    /// Returns the entry whose cLine == line, or nullptr.
    const LineMappingEntry* byCLine(int line) const;

private:
    QList<LineMappingEntry> entries_;
    QMap<int, int>          asmIndex_;
    QMap<int, int>          irIndex_;
    QMap<int, int>          cIndex_;
};

// ─── CodeSyntaxHighlighter ───────────────────────────────────────────────────

enum class PaneLang { Asm, IR, C, Cxx, CSharp, Java, Python, Lua };

/**
 * @brief Simple regex-based syntax highlighter for all three pane languages.
 */
class CodeSyntaxHighlighter : public QSyntaxHighlighter {
    Q_OBJECT
public:
    explicit CodeSyntaxHighlighter(QTextDocument* parent, PaneLang lang);

    void setLang(PaneLang lang);

protected:
    void highlightBlock(const QString& text) override;

private:
    struct Rule {
        QRegularExpression pattern;
        QTextCharFormat    format;
    };
    void buildRules();
    QList<Rule> rules_;
    PaneLang    lang_;

    // Multi-line comment state (for C/C++/Java/C#)
    QTextCharFormat commentFmt_;
    QRegularExpression commentStart_;
    QRegularExpression commentEnd_;
};

// ─── LineNumberArea (forward) ─────────────────────────────────────────────────

class SyncedCodePane;

class LineNumberArea : public QWidget {
    Q_OBJECT
public:
    explicit LineNumberArea(SyncedCodePane* pane);
    QSize sizeHint() const override;

protected:
    void paintEvent(QPaintEvent* event) override;

private:
    SyncedCodePane* pane_;
};

// ─── SyncedCodePane ───────────────────────────────────────────────────────────

/**
 * @brief Single code pane: line-number gutter + read-only editor + highlighter.
 *
 * Emits `lineActivated(line)` when the user clicks a line.
 * Emits `verticalScrollChanged(value)` when the view scrolls.
 * Accepts `scrollTo(value)` and `highlightLine(line)` from outside.
 */
class SyncedCodePane : public QWidget {
    Q_OBJECT
public:
    explicit SyncedCodePane(PaneLang lang, const QString& title,
                             QWidget* parent = nullptr);

    void setContent(const QString& text);
    void setLang(PaneLang lang);
    void highlightLine(int line);    ///< 1-based; -1 clears highlight
    void scrollToLine(int line);     ///< ensure line is centred in view
    void clearHighlight();

    QString title() const { return title_; }
    int lineCount() const;
    QPlainTextEdit* editor() const { return editor_; }  // returns CodeEditor*

    // Called by LineNumberArea
    int lineNumberAreaWidth() const;
    void lineNumberAreaPaintEvent(QPaintEvent* event);

signals:
    void lineActivated(int line);          ///< 1-based
    void verticalScrollChanged(int value);

private slots:
    void onCursorPositionChanged();
    void onScrollValueChanged(int value);
    void updateLineNumberAreaWidth(int newBlockCount);
    void updateLineNumberArea(const QRect& rect, int dy);

private:
    void setupUI();

    QString         title_;
    PaneLang        lang_;
    QLabel*         titleLabel_   = nullptr;
    CodeEditor*     editor_       = nullptr;
    LineNumberArea* lineNumArea_  = nullptr;
    CodeSyntaxHighlighter* hl_   = nullptr;
    int             highlightedLine_ = -1;
    bool            suppressScrollSignal_ = false;
    /// Prevents re-entrant updateLineNumberArea → updateLineNumberAreaWidth loops
    /// during QTextDocument layout / syntax rehighlight (seen as hangs in gui tests).
    bool            lineNumberAreaUpdateGuard_ = false;
};

// ─── TriPaneCodeView ──────────────────────────────────────────────────────────

/**
 * @brief Full tri-pane synchronized code browser panel.
 *
 * Hosts three `SyncedCodePane` instances (Asm | IR | C) inside a
 * `QSplitter`, and synchronizes cursor, highlighting, and scrolling.
 */
class TriPaneCodeView : public PanelBase {
    Q_OBJECT
public:
    explicit TriPaneCodeView(QWidget* parent = nullptr);

    // ── Load content ─────────────────────────────────────────────────────────

    void loadFunction(uint64_t address,
                      const QString& asmText,
                      const QString& irText,
                      const QString& cText);
    void setLineMapping(const LineMapping& mapping);
    void setOutputLanguage(OutputLang lang);

    // ── Navigation ───────────────────────────────────────────────────────────

    void navigateTo(uint64_t address);
    void navigateBack();
    void navigateForward();

    void clear() override;

    // ── Find ─────────────────────────────────────────────────────────────────

    void showFindBar();
    void hideFindBar();

signals:
    void addressNavigated(uint64_t address);
    void functionRequested(uint64_t address);

public slots:
    void onFunctionSelected(uint64_t address, const QString& name);

private slots:
    void onAsmLineActivated(int line);
    void onIrLineActivated(int line);
    void onCLineActivated(int line);
    void onAsmScrollChanged(int value);
    void onIrScrollChanged(int value);
    void onCScrollChanged(int value);
    void onFindTextChanged(const QString& text);
    void onFindNext();
    void onFindPrev();
    void onBackClicked();
    void onForwardClicked();
    void onLangChanged(int index);

private:
    void setupUI();
    QWidget* buildToolbar();
    void applyHighlight(int asmLine, int irLine, int cLine);
    void updateNavButtons();
    PaneLang outputLangToPaneLang(OutputLang lang) const;

    // ── Panes ─────────────────────────────────────────────────────────────────

    SyncedCodePane* asmPane_ = nullptr;
    SyncedCodePane* irPane_  = nullptr;
    SyncedCodePane* cPane_   = nullptr;
    QSplitter*      splitter_ = nullptr;

    // ── Toolbar ───────────────────────────────────────────────────────────────

    QPushButton* backBtn_    = nullptr;
    QPushButton* fwdBtn_     = nullptr;
    QLabel*      funcLabel_  = nullptr;
    QComboBox*   langCombo_  = nullptr;
    QLineEdit*   findEdit_   = nullptr;
    QPushButton* findClose_  = nullptr;
    QWidget*     findBar_    = nullptr;

    // ── State ─────────────────────────────────────────────────────────────────

    LineMapping  mapping_;
    OutputLang   outputLang_  = OutputLang::C;
    bool         syncingScroll_ = false;

    // Navigation history: list of (address, funcName) pairs
    struct NavEntry { uint64_t address; QString name; };
    QList<NavEntry> navHistory_;
    int             navIdx_    = -1;
    static constexpr int kMaxHistory = 32;
};

} // namespace retdec::gui::panels

#endif // RETDEC_GUI_PANELS_TRI_PANE_CODE_VIEW_H
