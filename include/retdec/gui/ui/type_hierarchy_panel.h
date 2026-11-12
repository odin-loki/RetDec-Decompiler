/**
 * @file include/retdec/gui/ui/type_hierarchy_panel.h
 * @brief Type-hierarchy browser with vtable viewer (FuseUI port of the Qt TypeHierarchyPanel).
 *
 * Layout: filter box; a vertical splitter with the class tree (table view: Class | Inherits | Vtable | Methods |
 * Compiler, indented rows with disclosure triangles, inheritance kinds colour-coded) above the vtable viewer
 * ("Vtable" title, bases / derived classes line, table # | Function | Address | Flags). "No RTTI classes recovered"
 * when empty.
 *
 * Selecting a class shows its vtable and reports the class name (for function-list filtering). Double-clicking (or
 * Enter on) a vtable slot with an address navigates to it. Clicking a row's triangle, double-clicking a row, or
 * Left / Right collapse and expand subtrees.
 *
 * @copyright (c) 2026 Odin Loch trading as Imortek
 */

#ifndef RETDEC_GUI_UI_TYPE_HIERARCHY_PANEL_H
#define RETDEC_GUI_UI_TYPE_HIERARCHY_PANEL_H

#include "retdec/gui/model/typehier_model.h"
#include "retdec/gui/model/types.h"

#include <fuse/ui/ui.hpp>
#include <fuse/ui/widgets/widgets.hpp>

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

namespace retdec::gui::ui {

class TypeHierarchyPanel {
public:
	TypeHierarchyPanel(fuse::ui::UiContext& ui, fuse::ui::NodeHandle parent);
	~TypeHierarchyPanel();
	TypeHierarchyPanel(const TypeHierarchyPanel&) = delete;
	TypeHierarchyPanel& operator=(const TypeHierarchyPanel&) = delete;

	fuse::ui::NodeHandle root() const
	{
		return m_root;
	}
	static constexpr const char* kTitle = "Type Hierarchy";

	// ── Data in ──────────────────────────────────────────────────────────────
	/// Replaces the hierarchy; the vtable viewer is cleared and the tree fully expanded.
	void setHierarchy(std::vector<model::ClassInfo> classes);
	/// Legacy compat (Qt ClassEntry): names, vtable, compiler, public parents.
	void setHierarchy(const std::vector<model::LegacyClassEntry>& entries);
	void clear();

	// ── Commands ─────────────────────────────────────────────────────────────
	/// Recursive case-insensitive filter on the class column (also typed into the filter box).
	void setFilter(const std::string& text);
	/// Selects (and reveals) a class; false when it is not shown.
	bool selectClass(std::string_view name);
	void setExpanded(std::string_view className, bool expanded);
	void expandAll();
	void collapseAll();
	/// Activates vtable row `row` (as a double click does).
	void activateVtableRow(int row);

	// ── State (tests, main window) ───────────────────────────────────────────
	int visibleRowCount() const
	{
		return int(m_rows.size());
	}
	/// Cell text of a visible tree row ("" out of range).
	std::string rowText(int row, int column) const;
	int rowDepth(int row) const;
	std::optional<std::string> selectedClass() const;
	int vtableRowCount() const
	{
		return m_vtable.rowCount();
	}
	std::string vtableText(int row, int column) const
	{
		return m_vtable.data(row, column);
	}
	std::string detailsText() const
	{
		return m_details;
	}
	bool emptyLabelVisible() const
	{
		return m_hierarchy.classes().empty();
	}
	const model::ClassHierarchyModel& hierarchy() const
	{
		return m_hierarchy;
	}
	const model::VtableModel& vtable() const
	{
		return m_vtable;
	}
	fuse::ui::NodeHandle searchBox() const
	{
		return m_search;
	}
	fuse::ui::NodeHandle treeTable() const
	{
		return m_tree;
	}
	fuse::ui::NodeHandle vtableTable() const
	{
		return m_vtableView;
	}
	/// Centre of a visible row of the tree / vtable table in context coordinates (valid after an update).
	std::optional<fuse::ui::Vec2> treeRowCenter(int row) const;
	std::optional<fuse::ui::Vec2> vtableRowCenter(int row) const;
	/// Centre of a tree row's disclosure triangle.
	std::optional<fuse::ui::Vec2> treeToggleCenter(int row) const;

	// ── Events out ───────────────────────────────────────────────────────────
	/// A class was selected (Qt classSelected) — for function-list filtering.
	std::function<void(const std::string& className)> onClassSelected;
	/// A vtable slot was activated (Qt vtableSlotNavigated).
	std::function<void(uint64_t address)> onVtableSlotActivated;
	/// Same address, as a generic navigation (Qt addressNavigated).
	std::function<void(uint64_t address)> onAddressNavigated;

	// Table glue (internal).
	struct Glue;

private:
	void rebuildRows(int keepNode);
	void classRowSelected(int row);
	void showVtable(int node);
	void toggleRow(int row);
	void treeClicked();
	void treeActivated(int row);
	bool overToggle(int row) const;
	std::optional<fuse::ui::Vec2> rowCenter(fuse::ui::NodeHandle table, int row) const;

	fuse::ui::UiContext& m_ui;
	fuse::ui::NodeHandle m_root, m_search, m_split, m_treePane, m_emptyLabel, m_tree;
	fuse::ui::NodeHandle m_vtablePane, m_vtableTitle, m_detailsLabel, m_vtableView;

	model::ClassHierarchyModel m_hierarchy;
	model::VtableModel m_vtable;
	std::vector<model::ClassHierarchyModel::Row> m_rows;
	std::unordered_set<int> m_collapsed;
	std::string m_filter, m_details;
	int m_selectedNode = -1;

	fuse::ui::widgets::ItemModel m_treeModel;
	fuse::ui::widgets::ItemModel m_vtableModel;
	fuse::ui::widgets::CellDelegate m_treeDelegate;
	fuse::ui::widgets::CellDelegate m_vtableDelegate;
};

} // namespace retdec::gui::ui

#endif // RETDEC_GUI_UI_TYPE_HIERARCHY_PANEL_H
