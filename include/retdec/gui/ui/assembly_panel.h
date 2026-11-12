/**
 * @file include/retdec/gui/ui/assembly_panel.h
 * @brief AssemblyPanel — syntax-highlighted disassembly view.
 *
 *   - address / mnemonic / register / immediate / comment colouring
 *   - toolbar: "Go to Address (G)", "Find (F)"; search bar below the view
 *   - context menu: Copy Address / Go to Function (disabled on lines
 *     without an address)
 *   - "No disassembly yet" empty state
 *
 * Keyboard (focus inside the panel): G go to address, F find, Enter follows
 * the jump / call target of the caret line (pushing the current address on a
 * back stack), Escape goes back (or hides the search bar when the stack is
 * empty).
 *
 * @copyright (c) 2026 Odin Loch trading as Imortek
 */

#ifndef RETDEC_GUI_UI_ASSEMBLY_PANEL_H
#define RETDEC_GUI_UI_ASSEMBLY_PANEL_H

#include "retdec/gui/ui/address_menu.h"
#include "retdec/gui/ui/empty_state.h"
#include "retdec/gui/ui/tri_pane_code_view.h"

#include <fuse/ui/ui.hpp>

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace retdec::gui::ui {

class AssemblyPanel {
public:
	enum class SearchMode
	{
		Find,
		GoTo,
	};

	AssemblyPanel(fuse::ui::UiContext& ui, fuse::ui::NodeHandle parent);
	~AssemblyPanel();
	AssemblyPanel(const AssemblyPanel&) = delete;
	AssemblyPanel& operator=(const AssemblyPanel&) = delete;

	fuse::ui::NodeHandle root() const
	{
		return root_;
	}
	std::string panelTitle() const
	{
		return "Assembly";
	}

	// ── Data in ──────────────────────────────────────────────────────────────
	void setAssemblyText(std::string_view text);
	/// Label "Address: 0x…" and scroll to the first line containing it (also
	/// re-applied when text arrives later).
	void navigateTo(uint64_t address);
	/// Slot for addressNavigated from other panels.
	void onAddressNavigated(uint64_t address)
	{
		navigateTo(address);
	}
	void applyEditorFont(float px);
	void clear();

	// ── Actions ──────────────────────────────────────────────────────────────
	/// Shows the search bar in go-to-address mode (G).
	void goToAddress();
	/// Shows the search bar in find mode (F).
	void find();
	void hideSearchBar();
	bool searchBarVisible() const
	{
		return searchShown_;
	}
	SearchMode searchMode() const
	{
		return mode_;
	}
	void setSearchText(std::string_view text);
	/// Enter in the search bar.
	void submitSearch();
	/// Enter on the view: follow the caret line's jump / call target.
	bool followJump();
	/// Escape on the view: back to the address before the last followed jump.
	bool navigateBack();
	std::size_t backStackSize() const
	{
		return navStack_.size();
	}
	void openContextMenu(fuse::ui::Vec2 at, int line);

	// ── Events out ───────────────────────────────────────────────────────────
	/// Context menu "Go to Function" (the Qt panel's addressNavigated signal).
	std::function<void(uint64_t address)> onAddressActivated;

	// ── Test hooks ───────────────────────────────────────────────────────────
	CodeView& view()
	{
		return *view_;
	}
	EmptyState& emptyState()
	{
		return *empty_;
	}
	bool emptyStateShown() const
	{
		return emptyShown_;
	}
	std::string functionLabel() const;
	fuse::ui::NodeHandle searchEdit() const
	{
		return searchEdit_;
	}
	fuse::ui::NodeHandle goToButton() const
	{
		return goToBtn_;
	}
	fuse::ui::NodeHandle findButton() const
	{
		return findBtn_;
	}
	ContextMenu& contextMenu()
	{
		return menu_;
	}
	/// 0-based caret line.
	int caretLine() const
	{
		return view_->caret().line;
	}

private:
	bool handleEvent(fuse::ui::EventContext& ec, const fuse::ui::Event& e);
	void scrollToAddress(uint64_t address);
	void updateEmptyState();
	void showSearch(SearchMode mode);

	fuse::ui::UiContext& ui_;
	PanelEventHandler handler_;
	ContextMenu menu_;
	fuse::ui::NodeHandle root_, goToBtn_, findBtn_, funcLabel_, body_, searchBar_, searchEdit_, searchLabel_;
	std::unique_ptr<EmptyState> empty_;
	std::unique_ptr<CodeView> view_;
	SearchMode mode_ = SearchMode::Find;
	bool searchShown_ = false;
	bool emptyShown_ = true;
	uint64_t pendingScrollAddr_ = 0;
	std::vector<uint64_t> navStack_;
};

} // namespace retdec::gui::ui

#endif // RETDEC_GUI_UI_ASSEMBLY_PANEL_H
