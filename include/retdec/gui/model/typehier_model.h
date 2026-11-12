/**
 * @file include/retdec/gui/model/typehier_model.h
 * @brief Class-hierarchy tree and vtable table models for the type-hierarchy panel (no UI).
 *
 * Port of the Qt ClassHierarchyModel / VtableModel (type_hierarchy_panel.cpp). A class hangs under the first of its
 * bases that is a known class; classes without a known base are roots. Columns: Class | Inherits | Vtable | Methods |
 * Compiler. The vtable table has # | Function | Address | Flags.
 *
 * @copyright (c) 2026 Odin Loch trading as Imortek
 */

#ifndef RETDEC_GUI_MODEL_TYPEHIER_MODEL_H
#define RETDEC_GUI_MODEL_TYPEHIER_MODEL_H

#include "retdec/gui/model/graph_scene.h"
#include "retdec/gui/model/types.h"

#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

namespace retdec::gui::model {

/// "public" / "protected" / "private" / "virtual".
const char* inheritLabel(InheritanceLink::Kind kind);
/// green / yellow / red / mauve.
Rgba inheritColor(InheritanceLink::Kind kind);

/// The Qt panel's legacy input: names, vtable, compiler and parent names (all public).
struct LegacyClassEntry
{
	std::string name;
	uint64_t vtableAddress = 0;
	std::string compiler;
	std::vector<std::string> parents;
};
std::vector<ClassInfo> classesFromLegacy(const std::vector<LegacyClassEntry>& entries);

class ClassHierarchyModel {
public:
	static constexpr int kColumns = 5;
	enum Column : int
	{
		ColName = 0,
		ColInherits = 1,
		ColVtable = 2,
		ColMethods = 3,
		ColCompiler = 4,
	};

	void setClasses(std::vector<ClassInfo> classes);
	const std::vector<ClassInfo>& classes() const
	{
		return m_classes;
	}

	// ── Tree (node indices; -1 = the invisible root) ─────────────────────────
	int rowCount(int parent = -1) const;
	int columnCount() const
	{
		return kColumns;
	}
	/// Node of child `row` under `parent`, or -1.
	int index(int row, int parent = -1) const;
	int parent(int node) const;
	int row(int node) const;
	int depth(int node) const;
	/// Node showing class `name`, or -1.
	int findClass(std::string_view name) const;
	/// nullptr for an invalid node.
	const ClassInfo* classAt(int node) const;
	/// Direct children's class names.
	std::vector<std::string> derivedClasses(int node) const;

	// ── Cells ────────────────────────────────────────────────────────────────
	/// Display text ("" for an invalid node). Name gets " [abstract]", empty values show "—".
	std::string data(int node, int column) const;
	Rgba foreground(int node, int column) const;
	bool italic(int node, int column) const;
	std::string tooltip(int node) const;
	static std::string headerData(int column);

	// ── Flattened view ───────────────────────────────────────────────────────
	struct Row
	{
		int node = -1;
		int depth = 0;
		bool hasChildren = false;
		bool expanded = true;
	};
	/// Depth-first rows. With a filter, a node shows when its name column contains the text (case-insensitive) or a
	/// descendant matches (Qt recursive filtering), and matching subtrees are shown expanded. `collapsed` holds
	/// nodes whose children are hidden.
	std::vector<Row> visibleRows(std::string_view filter, const std::unordered_set<int>& collapsed) const;
	bool matches(int node, std::string_view filter) const;

private:
	struct Node
	{
		int cls = -1;
		int parent = -1;
		int row = 0;
		int depth = 0;
		std::vector<int> children;
	};
	void buildTree();
	bool subtreeMatches(int node, std::string_view filter, std::vector<int8_t>& memo) const;

	std::vector<ClassInfo> m_classes;
	std::vector<Node> m_nodes; ///< one per class, same index
	std::vector<int> m_roots;
};

class VtableModel {
public:
	static constexpr int kColumns = 4;
	void setSlots(std::vector<VtableSlot> slots);
	const VtableSlot* slotAt(int row) const;
	int rowCount() const
	{
		return int(m_slots.size());
	}
	int columnCount() const
	{
		return kColumns;
	}
	std::string data(int row, int column) const;
	Rgba foreground(int row, int column) const;
	bool italic(int row, int column) const;
	std::string tooltip(int row) const;
	static std::string headerData(int column);

private:
	std::vector<VtableSlot> m_slots;
};

} // namespace retdec::gui::model

#endif // RETDEC_GUI_MODEL_TYPEHIER_MODEL_H
