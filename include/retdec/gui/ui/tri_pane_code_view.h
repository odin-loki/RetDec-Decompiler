/**
 * @file include/retdec/gui/ui/tri_pane_code_view.h
 * @brief CodeView (read-only, virtualised, syntax-coloured text view with a
 *        line-number gutter, used by every code / log panel), SyncedCodePane
 *        (titled CodeView) and TriPaneCodeView, the synchronised three-column
 *        Asm | IR | C browser.
 *
 * ## Tri-pane layout
 *
 *   [◀][▶]  func_name @ 0x401000            Lang: [C ▾]  [⌕]
 *   ┌ Assembly ─────────┬ SSA IR ──────────┬ Decompiled C ──────────┐
 *   │ 1│ 401000 push   │ 1│ %entry:       │ 1│ int sub_401000(…) { │
 *   └───────────────────┴──────────────────┴────────────────────────┘
 *   Find: [__________] [▲] [▼] [✕]
 *
 * Clicking a line (or moving the caret with the keyboard) in one pane looks it
 * up in the LineMapping and highlights + centres the mapped lines of the other
 * two; scrolling one pane scrolls the others proportionally. Back / forward
 * (Alt+Left / Alt+Right) walk the last 32 selected functions; Ctrl+F opens
 * the find bar (Enter / ▼ next, ▲ previous, Escape / ✕ closes).
 *
 * @copyright (c) 2026 Odin Loch trading as Imortek
 */

#ifndef RETDEC_GUI_UI_TRI_PANE_CODE_VIEW_H
#define RETDEC_GUI_UI_TRI_PANE_CODE_VIEW_H

#include "retdec/gui/model/code_highlight.h"
#include "retdec/gui/model/code_text.h"
#include "retdec/gui/model/console_log.h"
#include "retdec/gui/model/tri_pane_sync.h"
#include "retdec/gui/ui/empty_state.h"

#include <fuse/ui/ui.hpp>

#include <array>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace retdec::gui::ui {

// ── CodeView ─────────────────────────────────────────────────────────────────

/// Colours of a CodeView (Catppuccin Mocha by default, as the Qt views).
struct CodePalette
{
	fuse::ui::Color background, gutter, gutterText, gutterTextActive, text, placeholder;
	fuse::ui::Color currentLine, highlightLine, selection, caret, scrollThumb;
	std::array<fuse::ui::Color, model::kTokenKindCount> tokens{};
	/// Console line kinds (Plain, Warning, Error, Banner, Footer).
	std::array<fuse::ui::Color, 5> console{};
};
CodePalette defaultCodePalette();

/// Registers the "retdec-codeview" widget class (ensureRetdecUiClasses calls it).
void registerCodeViewClass(fuse::ui::UiContext& ui);

/**
 * Read-only code view: lines with an optional line-number gutter, token
 * colouring (model::highlightLine), caret + selection (mouse drag, Shift +
 * arrows, Ctrl+A), Ctrl+C copies the selection, an extra full-width line
 * highlight, find with wrap-around, vertical / horizontal scrolling (wheel,
 * Shift+wheel, keys, scrollbar drag). Only visible lines are painted.
 *
 * Content is either its own text (setText) or an external ConsoleBuffer
 * (setConsoleBuffer: lines coloured by their console classification).
 * Lines are 0-based in this API.
 */
class CodeView {
public:
	CodeView(fuse::ui::UiContext& ui, fuse::ui::NodeHandle parent, model::CodeLang lang = model::CodeLang::Plain);
	~CodeView();
	CodeView(const CodeView&) = delete;
	CodeView& operator=(const CodeView&) = delete;

	fuse::ui::NodeHandle node() const { return node_; }
	fuse::ui::UiContext& ui() const { return ui_; }

	// ── Content ──────────────────────────────────────────────────────────────
	void setText(std::string_view text);
	void clear() { setText({}); }
	std::string text() const;
	bool isEmpty() const;
	int lineCount() const;
	std::string_view line(int i) const;
	const std::vector<std::string>& lines() const { return lines_; }
	/// Shows @p buffer instead of own text (nullptr returns to own text). The
	/// buffer must outlive the view; call contentChanged() after appending.
	void setConsoleBuffer(const model::ConsoleBuffer* buffer);
	void contentChanged();

	void setLang(model::CodeLang lang);
	model::CodeLang lang() const { return lang_; }
	/// Token spans of line @p i as painted (empty for console / plain).
	const std::vector<model::TokenSpan>& tokens(int i) const;

	void setPalette(const CodePalette& p);
	const CodePalette& palette() const { return palette_; }
	void setShowGutter(bool on);
	void setPlaceholder(std::string_view text);
	std::string_view placeholder() const { return placeholder_; }
	/// Editor font size in px (applyEditorFont).
	void setFontSize(float px);

	// ── Caret, selection, highlight ──────────────────────────────────────────
	model::TextPos caret() const { return caret_; }
	model::TextRange selection() const { return {anchor_, caret_}; }
	std::string selectedText() const;
	/// Moves the caret (selecting from the anchor when @p extend). Programmatic
	/// moves notify onCaretLineChanged only when @p notify.
	void setCaret(model::TextPos p, bool extend = false, bool notify = false);
	void setSelection(model::TextRange r);
	void selectAll();
	/// Copies the selection to the clipboard; false when nothing is selected.
	bool copySelection();

	/// Full-width extra highlight of line @p line (-1 clears).
	void highlightLine(int line);
	int highlightedLine() const { return highlight_; }

	/// Finds @p query from the current selection (wrap-around); selects and
	/// centres the match. False when not found.
	bool find(std::string_view query, bool forward = true, bool caseSensitive = false);

	// ── Scrolling ────────────────────────────────────────────────────────────
	int firstVisibleLine() const { return first_; }
	/// Scrolls so @p line is the first visible line (clamped). Notifies
	/// onScrolled when @p notify.
	void setFirstVisibleLine(int line, bool notify = false);
	/// Centres @p line (clamped); does not notify.
	void centerOnLine(int line);
	/// Scrolls the minimum needed for @p line to be visible.
	void ensureVisible(int line);
	int visibleLineCount() const;
	/// Largest first-visible-line value (the scrollbar maximum).
	int scrollMax() const;
	void scrollToEnd();
	float scrollX() const { return scrollX_; }

	// ── Events ───────────────────────────────────────────────────────────────
	/// The caret moved to another line by user input (click, keys). 0-based.
	std::function<void(int line)> onCaretLineChanged;
	/// The view scrolled by user input (wheel, keys, scrollbar drag).
	std::function<void(int firstLine)> onScrolled;
	/// Right click at @p pos (context coordinates) over @p line (-1 below the text).
	std::function<void(fuse::ui::Vec2 pos, int line)> onContextMenu;
	/// Double click on @p line.
	std::function<void(int line)> onDoubleClicked;

	// Internal (widget class glue).
	struct Glue;

private:
	friend struct Glue;
	void retokenize();
	void clampCaret();
	model::TextPos hitTest(fuse::ui::Vec2 p) const;
	float lineHeight() const;
	float gutterWidth() const;
	void invalidate();
	void caretMoved(int oldLine, bool notify);

	fuse::ui::UiContext& ui_;
	fuse::ui::NodeHandle node_;
	model::CodeLang lang_;
	std::vector<std::string> lines_{std::string()};
	std::vector<std::vector<model::TokenSpan>> tokens_;
	const model::ConsoleBuffer* console_ = nullptr;
	std::uint64_t consoleDropped_ = 0;
	CodePalette palette_;
	std::string placeholder_;
	bool gutter_ = true;
	model::TextPos caret_, anchor_;
	int highlight_ = -1;
	int first_ = 0;
	int pendingCenter_ = -1; ///< centre request made before the first layout
	float scrollX_ = 0.f;
	bool dragging_ = false;
	bool dragScroll_ = false;
	float grab_ = 0.f;
	double lastClickTime_ = -1.0;
	int lastClickLine_ = -1;
};

// ── SyncedCodePane ───────────────────────────────────────────────────────────

/// Single pane of the tri-pane view: title strip + CodeView.
class SyncedCodePane {
public:
	SyncedCodePane(fuse::ui::UiContext& ui, fuse::ui::NodeHandle parent, model::CodeLang lang, std::string_view title);
	~SyncedCodePane();

	fuse::ui::NodeHandle root() const { return root_; }
	CodeView& editor() { return *view_; }
	const CodeView& editor() const { return *view_; }

	void setContent(std::string_view text);
	void setLang(model::CodeLang lang);
	model::CodeLang lang() const { return view_->lang(); }
	/// 1-based; < 1 clears.
	void highlightLine(int line);
	/// 1-based; centres the line and moves the caret there (no notification).
	void scrollToLine(int line);
	void clearHighlight();
	/// 1-based highlighted line, -1 when none.
	int highlightedLine() const;
	void setEditorFontSize(float px) { view_->setFontSize(px); }

	std::string title() const { return title_; }
	void setTitle(std::string_view title);
	/// Number of lines (QTextDocument block count: "a\n" has 2).
	int lineCount() const { return view_->lineCount(); }
	bool isReadOnly() const { return true; }

	/// User activated a line (1-based).
	std::function<void(int line)> onLineActivated;
	/// User scrolled (value = first visible line).
	std::function<void(int value)> onVerticalScrollChanged;

private:
	fuse::ui::UiContext& ui_;
	fuse::ui::NodeHandle root_, titleLabel_;
	std::unique_ptr<CodeView> view_;
	std::string title_;
};

// ── TriPaneCodeView ──────────────────────────────────────────────────────────

/// Decompiler output languages the C pane can colour.
enum class OutputLang : uint8_t
{
	C,
	Cxx,
	CSharp,
	Java,
	Python,
	Lua,
};
model::CodeLang toCodeLang(OutputLang lang);
OutputLang outputLangFromSettingsString(std::string_view s);

class TriPaneCodeView {
public:
	TriPaneCodeView(fuse::ui::UiContext& ui, fuse::ui::NodeHandle parent);
	~TriPaneCodeView();
	TriPaneCodeView(const TriPaneCodeView&) = delete;
	TriPaneCodeView& operator=(const TriPaneCodeView&) = delete;

	fuse::ui::NodeHandle root() const { return root_; }
	std::string panelTitle() const { return "Tri-Pane View"; }

	// ── Content ──────────────────────────────────────────────────────────────
	void loadFunction(uint64_t address, std::string_view asmText, std::string_view irText, std::string_view cText);
	void setLineMapping(const model::LineMapping& mapping);
	/// Derives the mapping from the addresses in the loaded texts.
	void buildLineMappingFromAddresses();
	const model::LineMapping& lineMapping() const { return mapping_; }
	void setOutputLanguage(OutputLang lang);
	OutputLang outputLanguage() const { return outputLang_; }
	/// Applies Settings -> Decompiler -> Output language ("c", "cpp", ...).
	void syncLanguageFromSettings(std::string_view settingsValue);
	void applyEditorFont(float px);
	void clear();

	// ── Navigation ───────────────────────────────────────────────────────────
	/// Requests navigation (emits onAddressNavigated).
	void navigateTo(uint64_t address);
	void navigateBack();
	void navigateForward();
	bool canGoBack() const { return history_.canGoBack(); }
	bool canGoForward() const { return history_.canGoForward(); }
	/// A function was selected elsewhere: label + history entry.
	void onFunctionSelected(uint64_t address, std::string_view name);

	// ── Find ─────────────────────────────────────────────────────────────────
	void showFindBar();
	void hideFindBar();
	bool findBarVisible() const { return findShown_; }
	void setFindText(std::string_view text);
	/// Finds in all three panes (wrap-around).
	void findNext();
	void findPrevious();

	// ── Events ───────────────────────────────────────────────────────────────
	std::function<void(uint64_t address)> onAddressNavigated;
	std::function<void(uint64_t address)> onFunctionRequested;
	std::function<void(const std::string& msg, int timeoutMs)> onStatusMessage;

	// ── Test hooks ───────────────────────────────────────────────────────────
	SyncedCodePane& asmPane() { return *asm_; }
	SyncedCodePane& irPane() { return *ir_; }
	SyncedCodePane& cPane() { return *c_; }
	SyncedCodePane& pane(model::TriPane p);
	std::string functionLabel() const;
	fuse::ui::NodeHandle findEdit() const { return findEdit_; }
	fuse::ui::NodeHandle langCombo() const { return langCombo_; }
	fuse::ui::NodeHandle backButton() const { return backBtn_; }
	fuse::ui::NodeHandle forwardButton() const { return fwdBtn_; }
	/// Simulates the user activating @p line (1-based) in @p pane.
	void activateLine(model::TriPane pane, int line);

private:
	void onLineActivated(model::TriPane pane, int line);
	void onScrollChanged(model::TriPane pane, int value);
	void applyHighlight(int asmLine, int irLine, int cLine);
	void updateNavButtons();
	bool handleEvent(fuse::ui::EventContext& ec, const fuse::ui::Event& e);

	fuse::ui::UiContext& ui_;
	PanelEventHandler handler_;
	fuse::ui::NodeHandle root_, backBtn_, fwdBtn_, funcLabel_, langCombo_, findBtn_;
	fuse::ui::NodeHandle findBar_, findEdit_, findPrev_, findNext_, findClose_;
	std::unique_ptr<SyncedCodePane> asm_, ir_, c_;
	model::LineMapping mapping_;
	OutputLang outputLang_ = OutputLang::C;
	bool syncingScroll_ = false;
	bool findShown_ = false;
	model::NavHistory history_{32};
};

} // namespace retdec::gui::ui

#endif // RETDEC_GUI_UI_TRI_PANE_CODE_VIEW_H
