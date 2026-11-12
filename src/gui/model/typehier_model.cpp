/**
 * @file src/gui/model/typehier_model.cpp
 * @brief Class-hierarchy tree and vtable models.
 * @copyright (c) 2026 Odin Loch trading as Imortek
 */

#include "retdec/gui/model/typehier_model.h"

#include "retdec/gui/model/graph_search.h"

#include <format>
#include <unordered_map>

namespace retdec::gui::model {

const char* inheritLabel(InheritanceLink::Kind kind)
{
	switch (kind)
	{
	case InheritanceLink::Kind::Public: return "public";
	case InheritanceLink::Kind::Protected: return "protected";
	case InheritanceLink::Kind::Private: return "private";
	case InheritanceLink::Kind::Virtual: return "virtual";
	}
	return "";
}

Rgba inheritColor(InheritanceLink::Kind kind)
{
	switch (kind)
	{
	case InheritanceLink::Kind::Public: return palette::green;
	case InheritanceLink::Kind::Protected: return palette::yellow;
	case InheritanceLink::Kind::Private: return palette::red;
	case InheritanceLink::Kind::Virtual: return palette::mauve;
	}
	return palette::subtext0;
}

std::vector<ClassInfo> classesFromLegacy(const std::vector<LegacyClassEntry>& entries)
{
	std::vector<ClassInfo> out;
	out.reserve(entries.size());
	for (const auto& e: entries)
	{
		ClassInfo ci;
		ci.name = e.name;
		ci.vtableAddress = e.vtableAddress;
		ci.compiler = e.compiler;
		for (const auto& p: e.parents)
			ci.bases.push_back(InheritanceLink{p, InheritanceLink::Kind::Public});
		out.push_back(std::move(ci));
	}
	return out;
}

// ── ClassHierarchyModel ──────────────────────────────────────────────────────

void ClassHierarchyModel::setClasses(std::vector<ClassInfo> classes)
{
	m_classes = std::move(classes);
	buildTree();
}

void ClassHierarchyModel::buildTree()
{
	m_nodes.assign(m_classes.size(), Node{});
	m_roots.clear();
	std::unordered_map<std::string, int> byName;
	for (int i = 0; i < int(m_classes.size()); ++i)
	{
		m_nodes[size_t(i)].cls = i;
		byName.emplace(m_classes[size_t(i)].name, i); // first class with a name wins
	}
	auto createsCycle = [&](int child, int parent) {
		for (int a = parent; a >= 0; a = m_nodes[size_t(a)].parent)
			if (a == child) return true;
		return false;
	};
	for (int i = 0; i < int(m_classes.size()); ++i)
	{
		for (const auto& link: m_classes[size_t(i)].bases)
		{
			const auto it = byName.find(link.base);
			if (it == byName.end() || it->second == i || createsCycle(i, it->second)) continue;
			m_nodes[size_t(i)].parent = it->second;
			break;
		}
	}
	// Children in input order, rows and depths.
	for (int i = 0; i < int(m_nodes.size()); ++i)
	{
		Node& n = m_nodes[size_t(i)];
		if (n.parent < 0)
		{
			n.row = int(m_roots.size());
			m_roots.push_back(i);
		}
		else
		{
			Node& p = m_nodes[size_t(n.parent)];
			n.row = int(p.children.size());
			p.children.push_back(i);
		}
	}
	std::vector<int> stack(m_roots.rbegin(), m_roots.rend());
	while (!stack.empty())
	{
		const int n = stack.back();
		stack.pop_back();
		for (int c: m_nodes[size_t(n)].children)
		{
			m_nodes[size_t(c)].depth = m_nodes[size_t(n)].depth + 1;
			stack.push_back(c);
		}
	}
}

int ClassHierarchyModel::rowCount(int parent) const
{
	if (parent < 0) return int(m_roots.size());
	if (parent >= int(m_nodes.size())) return 0;
	return int(m_nodes[size_t(parent)].children.size());
}

int ClassHierarchyModel::index(int row, int parent) const
{
	if (row < 0) return -1;
	if (parent < 0) return row < int(m_roots.size()) ? m_roots[size_t(row)] : -1;
	if (parent >= int(m_nodes.size())) return -1;
	const auto& ch = m_nodes[size_t(parent)].children;
	return row < int(ch.size()) ? ch[size_t(row)] : -1;
}

int ClassHierarchyModel::parent(int node) const
{
	return node >= 0 && node < int(m_nodes.size()) ? m_nodes[size_t(node)].parent : -1;
}

int ClassHierarchyModel::row(int node) const
{
	return node >= 0 && node < int(m_nodes.size()) ? m_nodes[size_t(node)].row : -1;
}

int ClassHierarchyModel::depth(int node) const
{
	return node >= 0 && node < int(m_nodes.size()) ? m_nodes[size_t(node)].depth : 0;
}

int ClassHierarchyModel::findClass(std::string_view name) const
{
	for (int i = 0; i < int(m_classes.size()); ++i)
		if (m_classes[size_t(i)].name == name) return i;
	return -1;
}

const ClassInfo* ClassHierarchyModel::classAt(int node) const
{
	return node >= 0 && node < int(m_classes.size()) ? &m_classes[size_t(node)] : nullptr;
}

std::vector<std::string> ClassHierarchyModel::derivedClasses(int node) const
{
	std::vector<std::string> out;
	if (node < 0 || node >= int(m_nodes.size())) return out;
	// Every class naming this one as a base (not only the tree children: multiple inheritance).
	const std::string& name = m_classes[size_t(node)].name;
	for (const auto& c: m_classes)
		for (const auto& b: c.bases)
			if (b.base == name)
			{
				out.push_back(c.name);
				break;
			}
	return out;
}

std::string ClassHierarchyModel::data(int node, int column) const
{
	const ClassInfo* c = classAt(node);
	if (!c) return {};
	switch (column)
	{
	case ColName: return c->isAbstract ? c->name + " [abstract]" : c->name;
	case ColInherits:
	{
		if (c->bases.empty()) return "—";
		std::string s;
		for (const auto& b: c->bases)
		{
			if (!s.empty()) s += ", ";
			s += std::format("{} {}", inheritLabel(b.kind), b.base);
		}
		return s;
	}
	case ColVtable: return c->vtableAddress ? hexAddress(c->vtableAddress) : "—";
	case ColMethods: return std::to_string(c->methodCount);
	case ColCompiler: return c->compiler.empty() ? "—" : c->compiler;
	default: return {};
	}
}

Rgba ClassHierarchyModel::foreground(int node, int column) const
{
	const ClassInfo* c = classAt(node);
	if (!c) return palette::text;
	switch (column)
	{
	case ColName: return c->isAbstract ? palette::mauve : palette::text;
	case ColInherits: return c->bases.empty() ? palette::overlay0 : inheritColor(c->bases.front().kind);
	case ColVtable: return palette::blue;
	case ColMethods: return palette::teal;
	case ColCompiler: return palette::subtext0;
	default: return palette::text;
	}
}

bool ClassHierarchyModel::italic(int node, int column) const
{
	const ClassInfo* c = classAt(node);
	return c && column == ColName && c->isAbstract;
}

std::string ClassHierarchyModel::tooltip(int node) const
{
	const ClassInfo* c = classAt(node);
	if (!c) return {};
	std::string tip = c->name;
	if (!c->moduleName.empty()) tip += "\nModule: " + c->moduleName;
	tip += std::format("\nVtable: 0x{:x}", c->vtableAddress);
	tip += std::format("\nMethods: {}", c->methodCount);
	for (const auto& b: c->bases)
		tip += std::format("\n  inherits {} {}", inheritLabel(b.kind), b.base);
	return tip;
}

std::string ClassHierarchyModel::headerData(int column)
{
	switch (column)
	{
	case ColName: return "Class";
	case ColInherits: return "Inherits";
	case ColVtable: return "Vtable";
	case ColMethods: return "Methods";
	case ColCompiler: return "Compiler";
	default: return {};
	}
}

bool ClassHierarchyModel::matches(int node, std::string_view filter) const
{
	return filter.empty() || containsNoCase(data(node, ColName), filter);
}

bool ClassHierarchyModel::subtreeMatches(int node, std::string_view filter, std::vector<int8_t>& memo) const
{
	int8_t& m = memo[size_t(node)];
	if (m >= 0) return m != 0;
	bool hit = matches(node, filter);
	for (int c: m_nodes[size_t(node)].children)
		hit = subtreeMatches(c, filter, memo) || hit;
	m = hit ? 1 : 0;
	return hit;
}

std::vector<ClassHierarchyModel::Row> ClassHierarchyModel::visibleRows(
	std::string_view filter, const std::unordered_set<int>& collapsed) const
{
	std::vector<Row> rows;
	std::vector<int8_t> memo(m_nodes.size(), -1);
	std::vector<int> stack(m_roots.rbegin(), m_roots.rend());
	while (!stack.empty())
	{
		const int n = stack.back();
		stack.pop_back();
		if (!filter.empty() && !subtreeMatches(n, filter, memo)) continue;
		const Node& nd = m_nodes[size_t(n)];
		bool anyChild = false;
		for (int c: nd.children)
			if (filter.empty() || subtreeMatches(c, filter, memo)) anyChild = true;
		const bool expanded = !collapsed.count(n);
		rows.push_back({n, nd.depth, anyChild, expanded});
		if (anyChild && expanded)
			for (auto it = nd.children.rbegin(); it != nd.children.rend(); ++it)
				stack.push_back(*it);
	}
	return rows;
}

// ── VtableModel ──────────────────────────────────────────────────────────────

void VtableModel::setSlots(std::vector<VtableSlot> slots)
{
	m_slots = std::move(slots);
}

const VtableSlot* VtableModel::slotAt(int row) const
{
	return row >= 0 && row < int(m_slots.size()) ? &m_slots[size_t(row)] : nullptr;
}

std::string VtableModel::data(int row, int column) const
{
	const VtableSlot* s = slotAt(row);
	if (!s) return {};
	switch (column)
	{
	case 0: return std::to_string(s->index);
	case 1: return s->funcName;
	case 2: return s->funcAddress ? hexAddress(s->funcAddress) : "—";
	case 3:
	{
		std::string flags;
		if (s->isPure) flags += "pure";
		if (s->isOverride) flags += flags.empty() ? "override" : " override";
		return flags.empty() ? "—" : flags;
	}
	default: return {};
	}
}

Rgba VtableModel::foreground(int row, int column) const
{
	const VtableSlot* s = slotAt(row);
	switch (column)
	{
	case 0: return palette::overlay0;
	case 1: return s && s->isPure ? palette::mauve : palette::text;
	case 2: return palette::blue;
	case 3: return palette::subtext0;
	default: return palette::text;
	}
}

bool VtableModel::italic(int row, int column) const
{
	const VtableSlot* s = slotAt(row);
	return s && column == 1 && s->isPure;
}

std::string VtableModel::tooltip(int row) const
{
	const VtableSlot* s = slotAt(row);
	if (!s) return {};
	return std::format("0x{:x}: {}{}", s->funcAddress, s->funcName, s->isPure ? " = 0" : "");
}

std::string VtableModel::headerData(int column)
{
	switch (column)
	{
	case 0: return "#";
	case 1: return "Function";
	case 2: return "Address";
	case 3: return "Flags";
	default: return {};
	}
}

} // namespace retdec::gui::model
