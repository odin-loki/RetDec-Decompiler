/**
 * @file src/gui/ui/type_hierarchy_panel.cpp
 * @brief FuseUI type-hierarchy panel: class tree table + vtable viewer.
 * @copyright (c) 2026 Odin Loch trading as Imortek
 */

#include "retdec/gui/ui/type_hierarchy_panel.h"
#include "retdec/gui/ui/theme.h"

#include "retdec/gui/ui/graph_view.h"
#include "retdec/gui/ui/empty_state.h"

#include <algorithm>
#include <cmath>
#include <format>

namespace retdec::gui::ui {

namespace w = fuse::ui::widgets;
using fuse::ui::NodeHandle;
using fuse::ui::Rect;
using fuse::ui::SignalArgs;
using fuse::ui::SignalKind;
using fuse::ui::UiContext;
using fuse::ui::Vec2;

namespace {
constexpr float kIndent = 16.f;
constexpr float kToggleW = 14.f;
constexpr float kCellPad = 6.f; ///< the table view's cell inset

float tableRowHeight(const UiContext& ui, NodeHandle n)
{
	const float px = ui.computed(n).fontSize;
	return std::ceil(ui.font() ? ui.font()->metrics(px).lineHeight() : px * 1.25f) + 6.f;
}
} // namespace

struct TypeHierarchyPanel::Glue
{
	static const TypeHierarchyPanel& self(const void* u)
	{
		return *static_cast<const TypeHierarchyPanel*>(u);
	}

	static uint32_t treeRows(const void* u)
	{
		return uint32_t(self(u).m_rows.size());
	}
	static std::string_view treeData(const void* u, uint32_t row, uint32_t col, std::string& scratch)
	{
		const auto& p = self(u);
		if (row >= p.m_rows.size()) return {};
		scratch = p.m_hierarchy.data(p.m_rows[row].node, int(col));
		return scratch;
	}
	static std::string_view treeHeader(const void*, uint32_t col)
	{
		static const std::string h[] = {
			model::ClassHierarchyModel::headerData(0),
			model::ClassHierarchyModel::headerData(1),
			model::ClassHierarchyModel::headerData(2),
			model::ClassHierarchyModel::headerData(3),
			model::ClassHierarchyModel::headerData(4)};
		return col < 5 ? std::string_view(h[col]) : std::string_view{};
	}
	static uint32_t vtRows(const void* u)
	{
		return uint32_t(self(u).m_vtable.rowCount());
	}
	static std::string_view vtData(const void* u, uint32_t row, uint32_t col, std::string& scratch)
	{
		scratch = self(u).m_vtable.data(int(row), int(col));
		return scratch;
	}
	static std::string_view vtHeader(const void*, uint32_t col)
	{
		static const std::string h[] = {
			model::VtableModel::headerData(0),
			model::VtableModel::headerData(1),
			model::VtableModel::headerData(2),
			model::VtableModel::headerData(3)};
		return col < 4 ? std::string_view(h[col]) : std::string_view{};
	}

	static void paintTree(
		const void* u,
		const fuse::ui::PaintContext& pc,
		fuse::ui::DrawList& dl,
		Rect cell,
		uint32_t row,
		uint32_t col,
		std::string_view text,
		bool)
	{
		const auto& p = self(u);
		if (row >= p.m_rows.size()) return;
		const auto& r = p.m_rows[row];
		const fuse::ui::Color color =
			pc.tint(graphColor(themedRgba(currentTheme(pc.ui), p.m_hierarchy.foreground(r.node, int(col)))));
		// Qt: monospace cells, abstract class names italic. FuseUI has no italic
		// faces: they are bold (and mauve, from the model's colour).
		const fuse::text::Font* face = styledFont(pc.ui, pc.node, true, p.m_hierarchy.italic(r.node, int(col)));
		if (col == 0)
		{
			const float x = cell.x + float(r.depth) * kIndent;
			if (r.hasChildren)
				pc.ui.paintString(
					dl,
					r.expanded ? "\xE2\x96\xBE" : "\xE2\x96\xB8",
					{x, cell.y, kToggleW, cell.h}, // ▾ ▸
					pc.tint(graphColor(themedRgba(currentTheme(pc.ui), model::catppuccin::overlay2))),
					pc.style.fontSize,
					fuse::ui::TextAlignment::Center,
					pc.node.index);
			const float tx = x + kToggleW + 2.f;
			pc.ui.paintString(
				dl,
				text,
				{tx, cell.y, std::max(0.f, cell.right() - tx), cell.h},
				color,
				pc.style.fontSize,
				fuse::ui::TextAlignment::Start,
				pc.node.index,
				face);
			return;
		}
		pc.ui.paintString(
			dl, text, cell, color, pc.style.fontSize, fuse::ui::TextAlignment::Start, pc.node.index, face);
	}
	static void paintVtable(
		const void* u,
		const fuse::ui::PaintContext& pc,
		fuse::ui::DrawList& dl,
		Rect cell,
		uint32_t row,
		uint32_t col,
		std::string_view text,
		bool)
	{
		const auto& p = self(u);
		// Monospace; pure-virtual slots italic in Qt -> bold here (no italic faces).
		pc.ui.paintString(
			dl,
			text,
			cell,
			pc.tint(graphColor(themedRgba(currentTheme(pc.ui), p.m_vtable.foreground(int(row), int(col))))),
			pc.style.fontSize,
			fuse::ui::TextAlignment::Start,
			pc.node.index,
			styledFont(pc.ui, pc.node, true, p.m_vtable.italic(int(row), int(col))));
	}
};

TypeHierarchyPanel::TypeHierarchyPanel(UiContext& ui, NodeHandle parent): m_ui(ui)
{
	m_treeModel = {this, 5, &Glue::treeRows, &Glue::treeData, &Glue::treeHeader};
	m_vtableModel = {this, 4, &Glue::vtRows, &Glue::vtData, &Glue::vtHeader};
	m_treeDelegate = {this, &Glue::paintTree};
	m_vtableDelegate = {this, &Glue::paintVtable};

	m_root = w::panel(ui, parent);
	ui.setCss(m_root, "flex-grow", "1");
	ui.setCss(m_root, "padding", "4px");
	ui.setCss(m_root, "gap", "4px");
	ui.addClass(m_root, "retdec-canvas");
	ui.setA11y(m_root, fuse::ui::A11yRole::Group, kTitle);

	m_search = w::lineEdit(ui, m_root, "");
	ui.addClass(m_search, "retdec-input");
	ui.setCss(m_search, "padding", "2px 6px");
	ui.setA11y(m_search, fuse::ui::A11yRole::TextField, "Filter classes…");
	w::setPlaceholderText(ui, m_search, "Filter classes\xE2\x80\xA6");

	m_split = w::splitter(ui, m_root, /*stacked=*/true);
	ui.setCss(m_split, "flex-grow", "1");
	m_treePane = w::splitterPane(ui, m_split, 0.f);
	ui.setCss(m_treePane, "padding", "0px");
	m_emptyLabel = w::label(ui, m_treePane, "No RTTI classes recovered");
	ui.addClass(m_emptyLabel, "retdec-faint");
	ui.setCss(m_emptyLabel, "text-align", "center");
	ui.setCss(m_emptyLabel, "padding", "24px");
	m_tree = w::tableView(ui, m_treePane, &m_treeModel);
	ui.setCss(m_tree, "flex-grow", "1");
	ui.setCss(m_tree, "font-size", "13px");
	const float treeW[] = {240.f, 220.f, 110.f, 70.f, 90.f};
	for (uint32_t c = 0; c < 5; ++c)
	{
		w::setTableColumnWidth(ui, m_tree, c, treeW[c]);
		w::setTableDelegate(ui, m_tree, c, &m_treeDelegate);
	}
	w::setTableActivation(ui, m_tree, true);

	m_vtablePane = w::splitterPane(ui, m_split, 220.f);
	ui.setCss(m_vtablePane, "padding", "0px");
	ui.setCss(m_vtablePane, "gap", "0px");
	m_vtableTitle = w::label(ui, m_vtablePane, "Vtable");
	ui.addClass(m_vtableTitle, "dim");
	ui.setCss(m_vtableTitle, "padding", "4px 6px 2px 6px");
	m_detailsLabel = w::label(ui, m_vtablePane, "");
	ui.addClass(m_detailsLabel, "retdec-faint");
	ui.setCss(m_detailsLabel, "font-size", "12px");
	ui.setCss(m_detailsLabel, "padding", "0px 6px 4px 6px");
	m_vtableView = w::tableView(ui, m_vtablePane, &m_vtableModel);
	ui.setCss(m_vtableView, "flex-grow", "1");
	ui.setCss(m_vtableView, "font-size", "13px");
	const float vtW[] = {40.f, 300.f, 130.f, 110.f};
	for (uint32_t c = 0; c < 4; ++c)
	{
		w::setTableColumnWidth(ui, m_vtableView, c, vtW[c]);
		w::setTableDelegate(ui, m_vtableView, c, &m_vtableDelegate);
	}
	w::setTableActivation(ui, m_vtableView, true);

	// ── Wiring ───────────────────────────────────────────────────────────────
	ui.connect(m_search, SignalKind::TextChanged, [this](UiContext&, NodeHandle, const SignalArgs& a) {
		setFilter(std::string(a.text));
	});
	ui.connect(m_tree, SignalKind::ValueChanged, [this](UiContext& u, NodeHandle n, const SignalArgs&) {
		classRowSelected(int(std::lround(u.value(n))));
	});
	ui.connect(m_tree, SignalKind::Clicked, [this](UiContext&, NodeHandle, const SignalArgs&) { treeClicked(); });
	ui.connect(m_tree, SignalKind::Event, [this](UiContext& u, NodeHandle n, const SignalArgs& a) {
		if (!a.event || a.event->type != fuse::ui::EventType::KeyDown) return;
		const int row = int(std::lround(u.value(n)));
		if (row < 0 || row >= int(m_rows.size())) return;
		if (a.event->key == fuse::ui::Key::Left)
		{
			if (m_rows[size_t(row)].hasChildren && m_rows[size_t(row)].expanded)
				toggleRow(row);
			else if (const int par = m_hierarchy.parent(m_rows[size_t(row)].node); par >= 0)
				selectClass(m_hierarchy.classAt(par)->name);
		}
		else if (
			a.event->key == fuse::ui::Key::Right && m_rows[size_t(row)].hasChildren && !m_rows[size_t(row)].expanded)
			toggleRow(row);
	});
	// Double click on a class row toggles it (not Enter: index -1); a double
	// click or Enter on a vtable slot navigates.
	ui.connect(m_tree, SignalKind::Activated, [this](UiContext&, NodeHandle, const SignalArgs& a) {
		if (a.index >= 0) treeActivated(int(std::lround(a.value)));
	});
	ui.connect(m_vtableView, SignalKind::Activated, [this](UiContext&, NodeHandle, const SignalArgs& a) {
		activateVtableRow(int(std::lround(a.value)));
	});

	rebuildRows(-1);
}

TypeHierarchyPanel::~TypeHierarchyPanel()
{
	if (m_ui.alive(m_root)) m_ui.destroy(m_root);
}

// ── Data ─────────────────────────────────────────────────────────────────────

void TypeHierarchyPanel::setHierarchy(std::vector<model::ClassInfo> classes)
{
	m_hierarchy.setClasses(std::move(classes));
	m_collapsed.clear(); // expandAll
	m_selectedNode = -1;
	showVtable(-1);
	rebuildRows(-1);
}

void TypeHierarchyPanel::setHierarchy(const std::vector<model::LegacyClassEntry>& entries)
{
	setHierarchy(model::classesFromLegacy(entries));
}

void TypeHierarchyPanel::clear()
{
	m_hierarchy.setClasses({});
	m_collapsed.clear();
	m_selectedNode = -1;
	m_filter.clear();
	m_ui.editBuffer(m_search).setText("");
	m_ui.setText(m_search, "");
	showVtable(-1);
	rebuildRows(-1);
}

void TypeHierarchyPanel::rebuildRows(int keepNode)
{
	m_rows = m_hierarchy.visibleRows(m_filter, m_collapsed);
	int sel = -1;
	for (int i = 0; i < int(m_rows.size()); ++i)
		if (m_rows[size_t(i)].node == keepNode) sel = i;
	m_ui.setValue(m_tree, float(sel));
	w::tableModelChanged(m_ui, m_tree);
	const bool empty = m_hierarchy.classes().empty();
	graphSetShown(m_ui, m_emptyLabel, empty);
	graphSetShown(m_ui, m_tree, !empty);
	m_ui.invalidate(m_tree);
}

// ── Commands ─────────────────────────────────────────────────────────────────

void TypeHierarchyPanel::setFilter(const std::string& text)
{
	if (std::string(m_ui.text(m_search)) != text)
	{
		m_ui.editBuffer(m_search).setText(text);
		m_ui.setText(m_search, text);
	}
	m_filter = text;
	rebuildRows(m_selectedNode); // Qt expandAll() after filtering: matches show expanded (visibleRows)
}

bool TypeHierarchyPanel::selectClass(std::string_view name)
{
	const int node = m_hierarchy.findClass(name);
	if (node < 0) return false;
	// Reveal: expand the ancestors.
	bool changed = false;
	for (int a = m_hierarchy.parent(node); a >= 0; a = m_hierarchy.parent(a))
		changed = m_collapsed.erase(a) > 0 || changed;
	if (changed) rebuildRows(m_selectedNode);
	for (int i = 0; i < int(m_rows.size()); ++i)
		if (m_rows[size_t(i)].node == node)
		{
			m_ui.setValue(m_tree, float(i), true); // ValueChanged -> classRowSelected
			if (m_selectedNode != node) classRowSelected(i);
			return true;
		}
	return false;
}

void TypeHierarchyPanel::setExpanded(std::string_view className, bool expanded)
{
	const int node = m_hierarchy.findClass(className);
	if (node < 0) return;
	if (expanded)
		m_collapsed.erase(node);
	else
		m_collapsed.insert(node);
	rebuildRows(m_selectedNode);
}

void TypeHierarchyPanel::expandAll()
{
	m_collapsed.clear();
	rebuildRows(m_selectedNode);
}

void TypeHierarchyPanel::collapseAll()
{
	for (int i = 0; i < int(m_hierarchy.classes().size()); ++i)
		if (m_hierarchy.rowCount(i) > 0) m_collapsed.insert(i);
	rebuildRows(m_selectedNode);
}

void TypeHierarchyPanel::toggleRow(int row)
{
	if (row < 0 || row >= int(m_rows.size()) || !m_rows[size_t(row)].hasChildren) return;
	const int node = m_rows[size_t(row)].node;
	if (!m_collapsed.erase(node)) m_collapsed.insert(node);
	rebuildRows(m_selectedNode);
}

// ── Selection ────────────────────────────────────────────────────────────────

void TypeHierarchyPanel::classRowSelected(int row)
{
	if (row < 0 || row >= int(m_rows.size())) return;
	const int node = m_rows[size_t(row)].node;
	m_selectedNode = node;
	showVtable(node);
	if (const model::ClassInfo* c = m_hierarchy.classAt(node); c && onClassSelected) onClassSelected(c->name);
}

void TypeHierarchyPanel::showVtable(int node)
{
	const model::ClassInfo* c = m_hierarchy.classAt(node);
	m_vtable.setSlots(c ? c->vtable : std::vector<model::VtableSlot>{});
	m_ui.setValue(m_vtableView, -1.f);
	w::tableModelChanged(m_ui, m_vtableView);
	m_ui.invalidate(m_vtableView);
	if (!c)
	{
		m_details.clear();
		m_ui.setText(m_vtableTitle, "Vtable");
	}
	else
	{
		m_ui.setText(
			m_vtableTitle,
			c->vtableAddress
				? std::format(
					  "Vtable — {} @ {} ({} slots)", c->name, model::hexAddress(c->vtableAddress), c->vtable.size())
				: std::format("Vtable — {} ({} slots)", c->name, c->vtable.size()));
		std::string bases;
		for (const auto& b: c->bases)
			bases += std::format("{}{} {}", bases.empty() ? "" : ", ", model::inheritLabel(b.kind), b.base);
		std::string derived;
		for (const auto& d: m_hierarchy.derivedClasses(node))
			derived += (derived.empty() ? "" : ", ") + d;
		m_details =
			std::format("Bases: {}   Derived: {}", bases.empty() ? "—" : bases, derived.empty() ? "—" : derived);
		if (!c->moduleName.empty()) m_details += "   Module: " + c->moduleName;
	}
	m_ui.setText(m_detailsLabel, m_details);
}

std::optional<std::string> TypeHierarchyPanel::selectedClass() const
{
	const model::ClassInfo* c = m_hierarchy.classAt(m_selectedNode);
	if (!c) return std::nullopt;
	return c->name;
}

std::string TypeHierarchyPanel::rowText(int row, int column) const
{
	if (row < 0 || row >= int(m_rows.size())) return {};
	return m_hierarchy.data(m_rows[size_t(row)].node, column);
}

int TypeHierarchyPanel::rowDepth(int row) const
{
	if (row < 0 || row >= int(m_rows.size())) return 0;
	return m_rows[size_t(row)].depth;
}

void TypeHierarchyPanel::treeClicked()
{
	const int row = int(std::lround(m_ui.value(m_tree)));
	if (row < 0 || row >= int(m_rows.size())) return;
	// Click on the disclosure triangle toggles.
	if (overToggle(row)) toggleRow(row);
}

bool TypeHierarchyPanel::overToggle(int row) const
{
	if (row < 0 || row >= int(m_rows.size()) || !m_rows[size_t(row)].hasChildren) return false;
	const auto t = treeToggleCenter(row);
	return t && std::abs(m_ui.pointer().x - t->x) <= kToggleW * 0.5f + 1.f;
}

void TypeHierarchyPanel::treeActivated(int row)
{
	// A double click on the triangle already toggled twice through its clicks.
	if (row < 0 || row >= int(m_rows.size()) || overToggle(row)) return;
	toggleRow(row);
}

void TypeHierarchyPanel::activateVtableRow(int row)
{
	const model::VtableSlot* s = m_vtable.slotAt(row);
	if (!s || s->funcAddress == 0) return;
	const uint64_t a = s->funcAddress;
	if (onVtableSlotActivated) onVtableSlotActivated(a);
	if (onAddressNavigated) onAddressNavigated(a);
}

// ── Geometry (tests) ─────────────────────────────────────────────────────────

std::optional<Vec2> TypeHierarchyPanel::rowCenter(NodeHandle table, int row) const
{
	if (!m_ui.alive(table)) return std::nullopt;
	const Rect c = m_ui.contentRect(table);
	const float rh = tableRowHeight(m_ui, table);
	const int first = int(w::tableFirstVisible(m_ui, table));
	const float y = c.y + rh /*header*/ + float(row - first) * rh + rh * 0.5f;
	if (row < first || y > c.bottom()) return std::nullopt;
	return Vec2{c.x + std::min(c.w * 0.5f, 120.f), y};
}

std::optional<Vec2> TypeHierarchyPanel::treeRowCenter(int row) const
{
	if (row < 0 || row >= int(m_rows.size())) return std::nullopt;
	return rowCenter(m_tree, row);
}

std::optional<Vec2> TypeHierarchyPanel::vtableRowCenter(int row) const
{
	if (row < 0 || row >= m_vtable.rowCount()) return std::nullopt;
	return rowCenter(m_vtableView, row);
}

std::optional<Vec2> TypeHierarchyPanel::treeToggleCenter(int row) const
{
	const auto c = treeRowCenter(row);
	if (!c) return std::nullopt;
	const Rect r = m_ui.contentRect(m_tree);
	return Vec2{r.x + kCellPad + float(m_rows[size_t(row)].depth) * kIndent + kToggleW * 0.5f, c->y};
}

} // namespace retdec::gui::ui
