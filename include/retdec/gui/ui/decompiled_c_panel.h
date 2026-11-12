/**
 * @file include/retdec/gui/ui/decompiled_c_panel.h
 * @brief DecompiledCPanel — read-only decompiled C output viewer.
 *
 *   - C syntax highlighting (or the configured output language), line numbers
 *   - toolbar: Copy, Save As…, Find, Refined (neural-refine sidecar toggle)
 *   - "No decompiled output yet" empty state
 *   - scroll to a function by line range, by name or by address
 *   - context menu: Copy, Select All, Find…, Copy Address, Go to Function
 *
 * Keyboard (focus inside the panel): Ctrl+F find bar, F3 / Shift+F3 next /
 * previous, Enter in the find bar searches, Escape hides the find bar.
 *
 * @copyright (c) 2026 Odin Loch trading as Imortek
 */

#ifndef RETDEC_GUI_UI_DECOMPILED_C_PANEL_H
#define RETDEC_GUI_UI_DECOMPILED_C_PANEL_H

#include "retdec/gui/model/code_highlight.h"
#include "retdec/gui/ui/address_menu.h"
#include "retdec/gui/ui/empty_state.h"
#include "retdec/gui/ui/tri_pane_code_view.h"

#include <fuse/ui/ui.hpp>

#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <string_view>

namespace retdec::gui::ui {

class DecompiledCPanel {
public:
	DecompiledCPanel(fuse::ui::UiContext& ui, fuse::ui::NodeHandle parent);
	~DecompiledCPanel();
	DecompiledCPanel(const DecompiledCPanel&) = delete;
	DecompiledCPanel& operator=(const DecompiledCPanel&) = delete;

	fuse::ui::NodeHandle root() const { return root_; }
	std::string panelTitle() const { return "Decompiled C"; }

	// ── Data in ──────────────────────────────────────────────────────────────
	void setSource(std::string_view cSource);
	/// Accepted neural-refine output (.refined.c). Non-empty enables the
	/// "Refined" toggle; empty disables it and shows the original.
	void setRefinedSource(std::string_view refined);
	void setShowRefined(bool on);
	bool showingRefined() const { return showingRefined_; }
	bool refinedAvailable() const { return !refined_.empty(); }
	/// Output language used for highlighting (default C).
	void setLanguage(model::CodeLang lang);
	void applyEditorFont(float px);
	void clear();

	/// Current editor text (diff / export helpers).
	std::string documentText() const;

	/// Scroll to a function body (1-based lines); falls back to `\bname\s*\(`.
	void scrollToFunction(std::string_view name, int startLine, int endLine = -1);
	/// Best-effort scroll to @p name, function_<hex> or 0x<hex>.
	void scrollToFunction(uint64_t address, std::string_view name);
	/// Label "name @ 0xaddr".
	void onFunctionSelected(uint64_t address, std::string_view name);

	// ── Actions ──────────────────────────────────────────────────────────────
	void showFindBar();
	void hideFindBar();
	bool findBarVisible() const { return findShown_; }
	void setFindText(std::string_view text);
	/// Enter in the find bar.
	void submitFind();
	/// F3 (opens the find bar when there is no query yet).
	void findNext();
	/// Shift+F3.
	void findPrevious();
	void copyToClipboard();
	/// Opens a save dialog (FuseUI file dialog) and saves there.
	void saveAs();
	/// Saves the shown text to @p path; reports "Saved: <path>" on success.
	bool saveTo(const std::filesystem::path& path);
	void openContextMenu(fuse::ui::Vec2 at);

	// ── Events out ───────────────────────────────────────────────────────────
	std::function<void(const std::string& msg, int timeoutMs)> onStatusMessage;
	/// Context menu "Go to Function" on a line with an address.
	std::function<void(uint64_t address)> onAddressActivated;

	// ── Test hooks ───────────────────────────────────────────────────────────
	CodeView& view() { return *view_; }
	EmptyState& emptyState() { return *empty_; }
	bool emptyStateShown() const { return emptyShown_; }
	std::string functionLabel() const;
	std::string lastFindQuery() const { return lastFindQuery_; }
	fuse::ui::NodeHandle findEdit() const { return findEdit_; }
	fuse::ui::NodeHandle copyButton() const { return copyBtn_; }
	fuse::ui::NodeHandle saveButton() const { return saveBtn_; }
	fuse::ui::NodeHandle findButton() const { return findBtn_; }
	fuse::ui::NodeHandle refinedToggle() const { return refinedChk_; }
	fuse::ui::NodeHandle saveDialog() const { return saveDlg_; }
	ContextMenu& contextMenu() { return menu_; }

private:
	bool handleEvent(fuse::ui::EventContext& ec, const fuse::ui::Event& e);
	void updateEmptyState();
	void status(const std::string& msg, int ms);
	void display(std::string_view text);

	fuse::ui::UiContext& ui_;
	PanelEventHandler handler_;
	ContextMenu menu_;
	fuse::ui::NodeHandle root_, copyBtn_, saveBtn_, findBtn_, refinedChk_, funcLabel_, body_, findEdit_, saveDlg_;
	std::unique_ptr<EmptyState> empty_;
	std::unique_ptr<CodeView> view_;
	std::string original_, refined_, lastFindQuery_;
	bool showingRefined_ = false;
	bool findShown_ = false;
	bool emptyShown_ = true;
};

} // namespace retdec::gui::ui

#endif // RETDEC_GUI_UI_DECOMPILED_C_PANEL_H
