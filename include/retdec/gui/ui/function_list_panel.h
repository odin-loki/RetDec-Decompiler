/**
 * @file include/retdec/gui/ui/function_list_panel.h
 * @brief Functions panel: sortable / filterable roster of recovered functions
 *        with recovery metadata (FuseUI port of the Qt FunctionListPanel).
 *
 * Layout
 *   [filter bar: name glob | address range | confidence >= | STL Crypto Algo
 *    Pat Lib | CSV JSON Copy]
 *   [table: Address | Name | Size | Instrs | Confidence | Patterns | CC | Notes]
 *   [status: "N functions" / "V / N functions"]
 *
 * Table behaviour: click a header to sort (again to reverse); click selects,
 * Ctrl+click toggles, Shift+click / Shift+arrows extend the selection, Ctrl+A
 * selects all; double-click or Enter activates (double-click on Name renames,
 * on Notes edits the notes); F2 renames; Ctrl+C copies the address; right-click
 * (or Shift+F10) opens the context menu: Go to Function, Re-decompile Selected
 * Function, Rename..., Edit signature..., Edit notes..., Clear annotation, Add
 * Tag to Selected..., Copy Address, Copy Signature, Export CSV, Export JSON.
 * The confidence column is a green / yellow / red bar, the patterns column five
 * coloured badges.
 *
 * Editing dialogs are FuseUI dialogs; exports go through a FuseUI file dialog
 * and forge::os::write_file. Nothing else touches files or processes.
 *
 * @copyright (c) 2026 Odin Loch trading as Imortek
 */

#ifndef RETDEC_GUI_UI_FUNCTION_LIST_PANEL_H
#define RETDEC_GUI_UI_FUNCTION_LIST_PANEL_H

#include "retdec/gui/model/funclist_filter.h"
#include "retdec/gui/model/funclist_model.h"
#include "retdec/gui/model/types.h"

#include <fuse/ui/ui.hpp>
#include <fuse/ui/widgets/widgets.hpp>

#include <array>
#include <cstdint>
#include <expected>
#include <functional>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <vector>

namespace retdec::gui::ui {

class FunctionListPanel {
public:
	static constexpr std::string_view kTitle = "Functions";

	/// Context-menu / keyboard actions.
	enum class Action
	{
		GoToFunction,    ///< onAddressActivated(address)
		Redecompile,     ///< onFunctionActivated + onRedecompileRequested(address, cli name)
		Rename,          ///< prompt for a new name
		EditSignature,   ///< prompt for the signature
		EditNotes,       ///< prompt for the notes
		ClearAnnotation, ///< notes, tags, signature cleared; raw name restored
		AddTag,          ///< prompt for a tag applied to every selected function
		CopyAddress,
		CopySignature,
		ExportCsv, ///< file dialog, then CSV of the visible rows
		ExportJson,
		CopyCsv, ///< CSV of the visible rows to the clipboard
	};

	FunctionListPanel(fuse::ui::UiContext& ui, fuse::ui::NodeHandle parent);
	~FunctionListPanel();
	FunctionListPanel(const FunctionListPanel&) = delete;
	FunctionListPanel& operator=(const FunctionListPanel&) = delete;

	fuse::ui::NodeHandle root() const
	{
		return root_;
	}

	// ── data in ─────────────────────────────────────────────────────────────
	void setFunctions(std::vector<model::FunctionEntry> fns);
	/// Drops the functions and resets every filter control.
	void clear();
	/// Show only "<className>::..." functions (Type Hierarchy); "" = all.
	void filterByClass(std::string_view className);
	/// Name column shows demangled names (default) or raw symbols.
	void setDemangleNames(bool on);
	/// Select (and scroll to) @p address and report it like an activation.
	void selectFunction(uint64_t address);

	// ── filter controls (also driven by the widgets) ────────────────────────
	std::string nameFilterText() const;
	void setNameFilterText(std::string_view text);
	void setAddressRangeText(std::string_view low, std::string_view high);
	void setMinConfidence(float minConfidence);
	/// Pattern check boxes, in order STL, Crypto, Algo, Pat (design), Lib.
	void setPatternFilter(bool stl, bool crypto, bool algo, bool design, bool lib);
	const model::FunctionFilter& filter() const
	{
		return proxy_.filter();
	}

	// ── state out ───────────────────────────────────────────────────────────
	const std::vector<model::FunctionEntry>& functions() const
	{
		return list_.functions();
	}
	std::optional<model::FunctionEntry> selectedEntry() const;
	/// Selected functions in view order.
	std::vector<uint64_t> selectedAddresses() const;
	std::string statusText() const;
	/// CSV / JSON of the visible rows, as the Qt export wrote them.
	std::string csvText() const;
	std::string jsonText() const;
	std::expected<void, std::string> exportCsv(const std::string& path) const;
	std::expected<void, std::string> exportJson(const std::string& path) const;

	// ── sorting ─────────────────────────────────────────────────────────────
	void sortBy(int column, bool ascending);
	int sortColumn() const
	{
		return proxy_.sortColumn();
	}
	bool sortAscending() const
	{
		return proxy_.sortAscending();
	}

	// ── actions ─────────────────────────────────────────────────────────────
	/// Runs @p a on @p address (default: the current row). Prompts open a dialog.
	void triggerAction(Action a, std::optional<uint64_t> address = std::nullopt);
	/// Opens the context menu at @p at for view row @p row (-1 = no row).
	void openContextMenu(fuse::ui::Vec2 at, int row);

	// ── events out ──────────────────────────────────────────────────────────
	/// Double-click / Enter / selectFunction / re-decompile: (address, name).
	std::function<void(uint64_t, const std::string&)> onFunctionActivated;
	/// "Go to Function".
	std::function<void(uint64_t)> onAddressActivated;
	/// (address, old name, new name) after a rename.
	std::function<void(uint64_t, const std::string&, const std::string&)> onFunctionRenamed;
	/// Selection changed (click, keys, programmatic).
	std::function<void()> onSelectionChanged;
	/// "Re-decompile Selected Function": (address, --select-functions name).
	std::function<void(uint64_t, const std::string&)> onRedecompileRequested;
	/// Notes, tags or signature of a function changed.
	std::function<void(const model::FunctionEntry&)> onAnnotationChanged;
	/// Clipboard writer; unset = UiContext::setClipboardText (the app binds
	/// the context's clipboard hooks to the system clipboard).
	std::function<bool(const std::string&)> clipboardWriter;

	// ── test hooks ──────────────────────────────────────────────────────────
	fuse::ui::NodeHandle tableNode() const
	{
		return table_;
	}
	fuse::ui::NodeHandle nameFilterNode() const
	{
		return nameEdit_;
	}
	fuse::ui::NodeHandle addrLowNode() const
	{
		return addrLowEdit_;
	}
	fuse::ui::NodeHandle addrHighNode() const
	{
		return addrHighEdit_;
	}
	fuse::ui::NodeHandle confidenceNode() const
	{
		return confSpin_;
	}
	fuse::ui::NodeHandle patternCheckNode(int i) const
	{
		return patternChecks_[static_cast<std::size_t>(i)];
	}
	fuse::ui::NodeHandle statusNode() const
	{
		return status_;
	}
	fuse::ui::NodeHandle emptyStateNode() const
	{
		return emptyState_;
	}
	fuse::ui::NodeHandle csvButtonNode() const
	{
		return csvBtn_;
	}
	fuse::ui::NodeHandle jsonButtonNode() const
	{
		return jsonBtn_;
	}
	fuse::ui::NodeHandle copyButtonNode() const
	{
		return copyBtn_;
	}
	std::size_t visibleRowCount() const
	{
		return proxy_.rowCount();
	}
	std::string cellText(std::size_t row, int col) const;
	uint64_t rowAddress(std::size_t row) const;
	/// Centre of a body cell / header cell in context coordinates (after update()).
	fuse::ui::Vec2 cellCenter(std::size_t row, int col) const;
	fuse::ui::Vec2 headerCenter(int col) const;
	bool promptOpen() const;
	fuse::ui::NodeHandle promptEdit() const
	{
		return promptEdit_;
	}
	/// Fills the open prompt with @p text and accepts it.
	void acceptPrompt(std::string_view text);
	void cancelPrompt();
	fuse::ui::NodeHandle contextMenu() const
	{
		return menu_;
	}
	/// Labels of the last context menu, in order (separators omitted).
	const std::vector<std::string>& contextMenuLabels() const
	{
		return menuLabels_;
	}
	fuse::ui::NodeHandle fileDialogNode() const
	{
		return fileDlg_;
	}
	/// The hover tooltip of the cell under the pointer (shown after 0.5 s).
	fuse::ui::NodeHandle cellToolTipNode() const
	{
		return cellTip_;
	}

private:
	enum class Prompt
	{
		None,
		Rename,
		Signature,
		Notes,
		Tag,
	};
	struct ColumnPainter
	{
		FunctionListPanel* panel = nullptr;
		int col = 0;
	};

	void build(fuse::ui::NodeHandle parent);
	void buildFilterBar(fuse::ui::NodeHandle bar);
	void buildPrompt();
	void tooltip(fuse::ui::NodeHandle n, std::string_view text);
	void onFilterChanged();
	void refresh(bool keepSelection);
	void updateStatus();
	void selectRow(int row, uint8_t mods);
	void setCurrentRow(int row, bool scroll);
	/// Marks the rows of selected_ in the table (after the rows moved) and sets its current row.
	void applySelectionToTable();
	/// selected_ / current_ from the table's selection (user input).
	void syncSelectionFromTable();
	void selectionChangedInternal();
	bool onTableEvent(const fuse::ui::Event& e);
	int rowAt(fuse::ui::Vec2 p) const;
	int columnAt(float x) const;
	void activateRow(int row, int col);
	void openPrompt(Prompt p, uint64_t address);
	void applyPrompt(const std::string& text);
	void openExportDialog(bool json);
	bool copy(const std::string& text);
	std::vector<const model::FunctionEntry*> visibleEntries() const;
	void notifyAnnotation(uint64_t address);
	void hoverCell(fuse::ui::Vec2 p);
	void hideCellTip();
	static void paintCell(
		const void* user,
		const fuse::ui::PaintContext& pc,
		fuse::ui::DrawList& dl,
		fuse::ui::Rect cell,
		fuse::ui::u32 row,
		fuse::ui::u32 col,
		std::string_view text,
		bool selected);

	fuse::ui::UiContext& ui_;
	model::FunctionList list_;
	model::FunctionListProxy proxy_{&list_};
	bool demangle_ = true;

	fuse::ui::widgets::ItemModel itemModel_{};
	std::array<ColumnPainter, model::FnColCount> painters_{};
	std::array<fuse::ui::widgets::CellDelegate, model::FnColCount> delegates_{};

	fuse::ui::NodeHandle root_, host_, table_, status_, emptyState_;
	fuse::ui::NodeHandle nameEdit_, addrLowEdit_, addrHighEdit_, confSpin_;
	std::array<fuse::ui::NodeHandle, 5> patternChecks_{};
	fuse::ui::NodeHandle csvBtn_, jsonBtn_, copyBtn_;
	fuse::ui::NodeHandle promptDlg_, promptTitle_, promptLabel_, promptEdit_;
	fuse::ui::NodeHandle menu_, fileDlg_;
	std::vector<fuse::ui::NodeHandle> tooltips_; ///< live in the overlay layer
	fuse::ui::NodeHandle cellTip_;
	fuse::ui::TimerHandle cellTipTimer_;
	bool cellTipPending_ = false;
	int tipRow_ = -1, tipCol_ = -1;
	fuse::ui::Vec2 tipPos_{};
	bool fileDlgJson_ = false;

	// selection (addresses; the table's value is the current row)
	std::set<uint64_t> selected_;
	std::optional<uint64_t> current_;
	int anchorRow_ = -1;
	bool applyingSelection_ = false;

	Prompt prompt_ = Prompt::None;
	uint64_t promptAddress_ = 0;
	std::vector<uint64_t> promptTargets_;

	std::vector<Action> menuActions_;
	std::vector<std::string> menuLabels_;
	std::optional<uint64_t> menuAddress_;

	std::shared_ptr<std::function<bool(fuse::ui::UiContext&, const fuse::ui::Event&)>> hook_;
};

} // namespace retdec::gui::ui

#endif // RETDEC_GUI_UI_FUNCTION_LIST_PANEL_H
