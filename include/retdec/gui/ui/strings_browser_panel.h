/**
 * @file include/retdec/gui/ui/strings_browser_panel.h
 * @brief Strings & Constants panel (FuseUI port of the Qt StringsBrowserPanel).
 *
 * Two tabs:
 *   Strings    Address | Type | Category | Preview (80 chars) | Refs
 *              toolbar: filter box (text, or /regex/), Type combo, Category
 *              combo, ".*" regex toggle, CSV export. Preview text is coloured
 *              by category (crypto amber, URL blue, format green, path yellow,
 *              regex mauve). Context menu: Copy Address, Copy Value.
 *   Constants  Address | Size | Hex | Decimal | Label | Refs
 *              toolbar: filter box (any column), label-kind combo (when a kind
 *              is chosen the text box is ignored, as in Qt), CSV export.
 *              Crypto constants amber, magic numbers blue, float specials green.
 *              Context menu: Copy Address, Copy Value (hex).
 * Click a header to sort; double-click or Enter reports the address
 * (onAddressActivated); Ctrl+C copies the address; Shift+F10 / right-click
 * opens the context menu.
 *
 * @copyright (c) 2026 Odin Loch trading as Imortek
 */

#ifndef RETDEC_GUI_UI_STRINGS_BROWSER_PANEL_H
#define RETDEC_GUI_UI_STRINGS_BROWSER_PANEL_H

#include "retdec/gui/model/strings_model.h"
#include "retdec/gui/model/types.h"

#include <fuse/ui/ui.hpp>
#include <fuse/ui/widgets/widgets.hpp>

#include <array>
#include <cstdint>
#include <expected>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace retdec::gui::ui {

class StringsBrowserPanel
{
public:
	static constexpr std::string_view kTitle = "Strings & Constants";

	enum class Table
	{
		Strings = 0,
		Constants = 1,
	};

	StringsBrowserPanel(fuse::ui::UiContext& ui, fuse::ui::NodeHandle parent);
	~StringsBrowserPanel();
	StringsBrowserPanel(const StringsBrowserPanel&) = delete;
	StringsBrowserPanel& operator=(const StringsBrowserPanel&) = delete;

	fuse::ui::NodeHandle root() const { return root_; }

	// ── data in ─────────────────────────────────────────────────────────────
	/// Entries still in the Plain category are classified.
	void setStrings(std::vector<model::StringEntry> entries);
	void appendStrings(std::vector<model::StringEntry> entries);
	/// Every constant is labelled (hex / decimal text, semantic label).
	void setConstants(std::vector<model::ConstantEntry> entries);
	/// Drops all rows and resets the filters.
	void clear();

	int stringCount() const { return static_cast<int>(strings_.size()); }
	int constantCount() const { return static_cast<int>(constants_.size()); }
	const model::StringsTable& strings() const { return strings_; }
	const model::ConstantsTable& constants() const { return constants_; }

	// ── filters (also driven by the widgets) ────────────────────────────────
	void setStringFilterText(std::string_view text);
	std::string stringFilterText() const;
	/// Type combo index: 0 = all, then ASCII, UTF-8, UTF-16 LE, UTF-16 BE, Wide, Pascal.
	void setTypeFilterIndex(int index);
	/// Category combo index: 0 = all, then Plain, FormatStr, URL, FilePath, RegEx, Crypto.
	void setCategoryFilterIndex(int index);
	void setRegexEnabled(bool on);
	bool regexEnabled() const;
	void setConstantFilterText(std::string_view text);
	/// Label-kind combo index (see model::constantKindFilterItems(); 0 = all).
	void setConstantKindIndex(int index);

	void sortBy(Table t, int column, bool ascending);

	/// 0 = Strings, 1 = Constants.
	void showTab(int index);
	int currentTab() const;

	// ── export ──────────────────────────────────────────────────────────────
	std::string stringsCsv() const { return strings_.exportCsv(); }
	std::string constantsCsv() const { return constants_.exportCsv(); }
	std::expected<void, std::string> exportStringsCsv(const std::string& path) const;
	std::expected<void, std::string> exportConstantsCsv(const std::string& path) const;
	/// Opens the save dialog of the CSV button of @p t.
	void openExportDialog(Table t);

	// ── events out ──────────────────────────────────────────────────────────
	/// Double-click / Enter on a row of either tab.
	std::function<void(uint64_t)> onAddressActivated;
	/// Clipboard writer; unset = UiContext::setClipboardText.
	std::function<bool(const std::string&)> clipboardWriter;

	// ── test hooks ──────────────────────────────────────────────────────────
	fuse::ui::NodeHandle tableNode(Table t) const { return tables_[idx(t)].table; }
	fuse::ui::NodeHandle searchNode(Table t) const { return tables_[idx(t)].search; }
	fuse::ui::NodeHandle statusNode(Table t) const { return tables_[idx(t)].status; }
	fuse::ui::NodeHandle exportButtonNode(Table t) const { return tables_[idx(t)].exportBtn; }
	fuse::ui::NodeHandle typeComboNode() const { return typeCombo_; }
	fuse::ui::NodeHandle categoryComboNode() const { return catCombo_; }
	fuse::ui::NodeHandle regexButtonNode() const { return regexBtn_; }
	fuse::ui::NodeHandle kindComboNode() const { return kindCombo_; }
	fuse::ui::NodeHandle tabsNode() const { return tabs_; }
	fuse::ui::NodeHandle contextMenu() const { return menu_; }
	fuse::ui::NodeHandle fileDialogNode() const { return fileDlg_; }
	const std::vector<std::string>& contextMenuLabels() const { return menuLabels_; }
	/// Hover tooltip of the cell under the pointer (full string value, constant label).
	fuse::ui::NodeHandle cellToolTipNode() const { return cellTip_; }
	std::size_t visibleRowCount(Table t) const;
	std::string cellText(Table t, std::size_t row, int col) const;
	uint64_t rowAddress(Table t, std::size_t row) const;
	std::string statusText(Table t) const;
	fuse::ui::Vec2 cellCenter(Table t, std::size_t row, int col) const;
	fuse::ui::Vec2 headerCenter(Table t, int col) const;
	/// Opens the context menu of @p t at @p at for view row @p row.
	void openContextMenu(Table t, fuse::ui::Vec2 at, int row);

private:
	struct TableKit;
	struct Painter
	{
		StringsBrowserPanel* panel = nullptr;
		int table = 0;
	};
	static constexpr std::size_t idx(Table t) { return static_cast<std::size_t>(t); }

	void build(fuse::ui::NodeHandle parent);
	void buildTable(Table t, fuse::ui::NodeHandle page);
	void tooltip(fuse::ui::NodeHandle n, std::string_view text);
	void applyStringFilter();
	void applyConstantFilter();
	void refresh(Table t);
	void updateStatus();
	bool onTableEvent(Table t, const fuse::ui::Event& e);
	int rowAt(Table t, fuse::ui::Vec2 p) const;
	void activate(Table t, int row);
	void copyRow(Table t, int row, bool value);
	bool copy(const std::string& text);
	int columnAt(Table t, float x) const;
	void hoverCell(Table t, fuse::ui::Vec2 p);
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
	model::StringsTable strings_;
	model::ConstantsTable constants_;
	model::StringsProxy strProxy_{&strings_};
	model::ConstantsProxy constProxy_{&constants_};

	struct TableKit
	{
		fuse::ui::NodeHandle host, table, search, status, exportBtn;
		fuse::ui::widgets::ItemModel model{};
		Painter painter{};
		fuse::ui::widgets::CellDelegate delegate{};
		int lastClickRow = -1;
		double lastClickTime = -1.0;
		std::shared_ptr<std::function<bool(fuse::ui::UiContext&, const fuse::ui::Event&)>> hook;
	};
	std::array<TableKit, 2> tables_{};

	fuse::ui::NodeHandle root_, tabs_, typeCombo_, catCombo_, regexBtn_, kindCombo_;
	fuse::ui::NodeHandle menu_, fileDlg_;
	Table fileDlgTable_ = Table::Strings;
	Table menuTable_ = Table::Strings;
	int menuRow_ = -1;
	std::vector<std::string> menuLabels_;
	std::vector<fuse::ui::NodeHandle> tooltips_;
	fuse::ui::NodeHandle cellTip_;
	fuse::ui::TimerHandle cellTipTimer_;
	bool cellTipPending_ = false;
	Table tipTable_ = Table::Strings;
	int tipRow_ = -1, tipCol_ = -1;
	fuse::ui::Vec2 tipPos_{};
};

} // namespace retdec::gui::ui

#endif // RETDEC_GUI_UI_STRINGS_BROWSER_PANEL_H
