/**
 * @file src/gui/ui/call_graph_panel.cpp
 * @brief FuseUI call-graph panel.
 * @copyright (c) 2026 Odin Loch trading as Imortek
 */

#include "retdec/gui/ui/call_graph_panel.h"

#include "retdec/gui/model/graph_export.h"
#include "retdec/gui/model/graph_search.h"

#include <fuse/ui/widgets/widgets.hpp>

#include <algorithm>
#include <cmath>
#include <format>

namespace retdec::gui::ui {

namespace w = fuse::ui::widgets;
using fuse::ui::NodeHandle;
using fuse::ui::SignalArgs;
using fuse::ui::SignalKind;
using fuse::ui::UiContext;

namespace {

NodeHandle smallSpin(UiContext& ui, NodeHandle parent, float lo, float hi, float v, const char* width)
{
	const NodeHandle s = w::spinBox(ui, parent, lo, hi, v, 1.f);
	ui.setCss(s, "width", width);
	ui.setCss(s, "font-size", "12px");
	ui.setCss(s, "padding", "2px 6px");
	return s;
}

} // namespace

CallGraphPanel::CallGraphPanel(UiContext& ui, NodeHandle parent): m_ui(ui)
{
	m_root = w::panel(ui, parent);
	ui.setCss(m_root, "flex-grow", "1");
	ui.setCss(m_root, "gap", "0px");
	ui.setCss(m_root, "padding", "0px");
	ui.addClass(m_root, "retdec-canvas");
	ui.setA11y(m_root, fuse::ui::A11yRole::Group, kTitle);

	// ── Filter row ───────────────────────────────────────────────────────────
	const NodeHandle row1 = graphToolbarRow(ui, m_root);
	m_nameFilter = w::lineEdit(ui, row1, "");
	ui.setCss(m_nameFilter, "width", "160px");
	ui.setCss(m_nameFilter, "font-size", "12px");
	ui.setCss(m_nameFilter, "padding", "2px 6px");
	ui.setA11y(m_nameFilter, fuse::ui::A11yRole::TextField, "Filter functions");
	w::setPlaceholderText(ui, m_nameFilter, "Filter functions\xE2\x80\xA6");
	w::setTooltip(ui, m_nameFilter, "Substring, or a glob with * and ?");
	graphMutedLabel(ui, row1, "Min calls:");
	m_minCalls = smallSpin(ui, row1, 0.f, 100000.f, 0.f, "70px");
	graphMutedLabel(ui, row1, "Depth:");
	m_depth = smallSpin(ui, row1, 0.f, 99.f, 0.f, "56px");
	w::setSpecialValueText(ui, m_depth, "All"); // 0 = no depth limit (Qt: specialValueText)
	w::setTooltip(ui, m_depth, "Maximum call depth from the roots");
	m_hideLib = w::checkBox(ui, row1, "Hide lib", false);
	ui.setCss(m_hideLib, "font-size", "12px");
	m_module = w::comboBox(ui, row1, "All modules", 0);
	ui.setCss(m_module, "font-size", "12px");
	ui.setCss(m_module, "padding", "2px 6px");
	graphMutedLabel(ui, row1, "Focus hops:");
	m_hops = smallSpin(ui, row1, 1.f, 10.f, 2.f, "48px");
	m_clearFocus = graphToolButton(ui, row1, "Clear Focus");
	ui.setDisabled(m_clearFocus, true);

	// ── View row ─────────────────────────────────────────────────────────────
	const NodeHandle row2 = graphToolbarRow(ui, m_root);
	m_find = w::lineEdit(ui, row2, "");
	ui.setCss(m_find, "width", "140px");
	ui.setCss(m_find, "font-size", "12px");
	ui.setCss(m_find, "padding", "2px 6px");
	ui.setA11y(m_find, fuse::ui::A11yRole::TextField, "Find function");
	w::setPlaceholderText(ui, m_find, "Find function\xE2\x80\xA6");
	w::setTooltip(ui, m_find, "Highlight matching functions (Enter centres the first)");
	m_layoutCombo = w::comboBox(ui, row2, "Force-directed\nLayered", 0);
	ui.setCss(m_layoutCombo, "font-size", "12px");
	ui.setCss(m_layoutCombo, "padding", "2px 6px");
	m_collapse = w::checkBox(ui, row2, "Collapse SCCs", true);
	ui.setCss(m_collapse, "font-size", "12px");
	m_minimapCheck = w::checkBox(ui, row2, "Minimap", true);
	ui.setCss(m_minimapCheck, "font-size", "12px");
	const NodeHandle spacer = w::panel(ui, row2);
	ui.setCss(spacer, "flex-grow", "1");
	m_focusLabel = graphMutedLabel(ui, row2, "");
	ui.removeClass(m_focusLabel, "dim");
	ui.addClass(m_focusLabel, "retdec-accent");
	m_infoLabel = graphMutedLabel(ui, row2, "");
	m_fitBtn = graphToolButton(ui, row2, "Fit");
	m_resetBtn = graphToolButton(ui, row2, "100%");
	m_dotBtn = graphToolButton(ui, row2, "Export DOT");
	m_svgBtn = graphToolButton(ui, row2, "Export SVG");
	m_pngBtn = graphToolButton(ui, row2, "Export PNG");

	m_view = std::make_unique<GraphView>(ui, m_root);
	m_view->setEmptyText("No call graph");

	// ── Wiring ───────────────────────────────────────────────────────────────
	ui.connect(m_nameFilter, SignalKind::TextChanged, [this](UiContext&, NodeHandle, const SignalArgs& a) {
		setNameFilter(std::string(a.text));
	});
	ui.connect(m_find, SignalKind::TextChanged, [this](UiContext&, NodeHandle, const SignalArgs& a) {
		search(std::string(a.text));
	});
	ui.connect(m_find, SignalKind::Submitted, [this](UiContext&, NodeHandle, const SignalArgs& a) {
		search(std::string(a.text));
	});
	ui.connect(m_minCalls, SignalKind::ValueChanged, [this](UiContext&, NodeHandle, const SignalArgs& a) {
		if (!m_updating) setMinCallCount(int(std::lround(a.value)));
	});
	ui.connect(m_depth, SignalKind::ValueChanged, [this](UiContext&, NodeHandle, const SignalArgs& a) {
		if (!m_updating) setCallGraphDepth(int(std::lround(a.value)));
	});
	ui.connect(m_hideLib, SignalKind::Toggled, [this](UiContext&, NodeHandle, const SignalArgs& a) {
		if (!m_updating) setHideLibrary(a.flag);
	});
	ui.connect(m_module, SignalKind::ValueChanged, [this](UiContext& u, NodeHandle n, const SignalArgs&) {
		if (m_updating) return;
		const int idx = int(std::lround(u.value(n)));
		setModuleFilter(idx <= 0 || idx > int(m_moduleIds.size()) ? -1 : m_moduleIds[size_t(idx - 1)]);
	});
	ui.connect(m_layoutCombo, SignalKind::ValueChanged, [this](UiContext& u, NodeHandle n, const SignalArgs&) {
		if (!m_updating)
			setLayoutMode(
				u.value(n) >= 0.5f ? model::CallGraphLayoutMode::Layered : model::CallGraphLayoutMode::ForceDirected);
	});
	ui.connect(m_collapse, SignalKind::Toggled, [this](UiContext&, NodeHandle, const SignalArgs& a) {
		if (!m_updating) setCollapseSccs(a.flag);
	});
	ui.connect(m_minimapCheck, SignalKind::Toggled, [this](UiContext&, NodeHandle, const SignalArgs& a) {
		m_view->setMinimapVisible(a.flag);
	});
	ui.connect(m_clearFocus, SignalKind::Clicked, [this](UiContext&, NodeHandle, const SignalArgs&) { clearFocus(); });
	ui.connect(m_fitBtn, SignalKind::Clicked, [this](UiContext&, NodeHandle, const SignalArgs&) { fitView(); });
	ui.connect(m_resetBtn, SignalKind::Clicked, [this](UiContext&, NodeHandle, const SignalArgs&) { resetZoom(); });
	ui.connect(m_dotBtn, SignalKind::Clicked, [this](UiContext&, NodeHandle, const SignalArgs&) {
		requestExport(GraphExportFormat::Dot);
	});
	ui.connect(m_svgBtn, SignalKind::Clicked, [this](UiContext&, NodeHandle, const SignalArgs&) {
		requestExport(GraphExportFormat::Svg);
	});
	ui.connect(m_pngBtn, SignalKind::Clicked, [this](UiContext&, NodeHandle, const SignalArgs&) {
		requestExport(GraphExportFormat::Png);
	});

	m_view->onNodeClicked = [this](uint64_t id) { nodeClicked(id); };
	m_view->onNodeActivated = [this](uint64_t id) {
		if (m_model.isSccNode(id))
			m_ui.post(
				[](UiContext& u, NodeHandle n, uint64_t self, uint64_t) {
					if (u.alive(n)) reinterpret_cast<CallGraphPanel*>(self)->setCollapseSccs(false);
				},
				m_root,
				reinterpret_cast<uint64_t>(this));
	};
	m_view->onNodeContextMenu = [this](uint64_t id, fuse::ui::Vec2 at) {
		std::string items;
		for (const auto& it: nodeMenuItems(id))
			items += (items.empty() ? "" : "\n") + it;
		m_view->openMenu(items, at, [this, id](int index) { runNodeMenu(id, index); });
	};
	refreshLabels();
}

CallGraphPanel::~CallGraphPanel()
{
	m_view.reset();
	if (m_prompt && m_ui.alive(m_prompt->dialog())) m_ui.destroy(m_prompt->dialog());
	m_prompt.reset();
	if (m_ui.alive(m_root)) m_ui.destroy(m_root);
}

// ── Data ─────────────────────────────────────────────────────────────────────

void CallGraphPanel::setCallGraph(std::vector<model::CallGraphNode> nodes, std::vector<model::CallEdge> edges)
{
	const size_t count = nodes.size();
	m_model.loadGraph(std::move(nodes), std::move(edges));
	rebuildModuleCombo();
	m_infoText = std::format("{} nodes", count);
	refresh(true);
}

void CallGraphPanel::setCallGraph(const std::vector<std::string>& names, std::vector<model::CallEdge> edges)
{
	std::vector<model::CallGraphNode> nodes;
	nodes.reserve(names.size());
	for (size_t i = 0; i < names.size(); ++i)
	{
		model::CallGraphNode n;
		n.address = i;
		n.name = names[i];
		nodes.push_back(std::move(n));
	}
	setCallGraph(std::move(nodes), std::move(edges));
}

void CallGraphPanel::rebuildModuleCombo()
{
	const int previous = m_model.moduleFilter();
	m_moduleIds = m_model.moduleIds();
	std::string items = "All modules";
	int restore = 0;
	for (size_t i = 0; i < m_moduleIds.size(); ++i)
	{
		items += std::format("\nModule {}", m_moduleIds[i]);
		if (m_moduleIds[i] == previous) restore = int(i + 1);
	}
	m_updating = true;
	m_ui.setText(m_module, items);
	m_ui.setRange(m_module, 0.f, float(m_moduleIds.size()), 1.f);
	m_ui.setValue(m_module, float(restore));
	m_updating = false;
	m_model.setModuleFilter(restore == 0 ? -1 : m_moduleIds[size_t(restore - 1)]);
}

std::vector<std::string> CallGraphPanel::moduleItems() const
{
	std::vector<std::string> out{"All modules"};
	for (int id: m_moduleIds)
		out.push_back(std::format("Module {}", id));
	return out;
}

void CallGraphPanel::refresh(bool fit)
{
	m_view->setScene(m_model.scene(), fit);
	refreshLabels();
}

void CallGraphPanel::filtersChanged()
{
	// Visibility only: reuse the view's scene geometry, copy the flags.
	model::GraphScene& sc = m_view->mutableScene();
	const model::GraphScene& src = m_model.scene();
	if (sc.nodes.size() != src.nodes.size() || sc.edges.size() != src.edges.size()
		|| sc.regions.size() != src.regions.size())
	{
		refresh(false);
	}
	else
	{
		for (size_t i = 0; i < sc.nodes.size(); ++i)
			sc.nodes[i].visible = src.nodes[i].visible;
		for (size_t i = 0; i < sc.edges.size(); ++i)
			sc.edges[i].visible = src.edges[i].visible;
		for (size_t i = 0; i < sc.regions.size(); ++i)
		{
			sc.regions[i].visible = src.regions[i].visible;
			sc.regions[i].rect = src.regions[i].rect;
		}
		m_view->sceneChanged();
	}
	m_infoText = std::format("{} visible", m_model.visibleNodeCount());
	refreshLabels();
}

void CallGraphPanel::refreshLabels()
{
	m_ui.setText(m_infoLabel, infoLabel());
	m_ui.setText(m_focusLabel, focusLabel());
	m_ui.setDisabled(m_clearFocus, !m_model.isFocused());
}

std::string CallGraphPanel::infoLabel() const
{
	return m_infoText;
}

std::string CallGraphPanel::focusLabel() const
{
	return m_focusText;
}

bool CallGraphPanel::clearFocusEnabled() const
{
	return !m_ui.disabled(m_clearFocus);
}

void CallGraphPanel::setFunction(uint64_t address, const std::string& name)
{
	if (!m_model.isFocused()) m_focusText = name;
	if (const auto id = m_model.sceneNodeFor(address))
	{
		m_view->setSelectedNode(*id);
		if (const auto* n = m_view->scene().node(*id); n && n->visible) m_view->centerOn(*id);
	}
	refreshLabels();
}

void CallGraphPanel::clear()
{
	m_model.clearGraph();
	m_updating = true;
	m_ui.editBuffer(m_nameFilter).setText("");
	m_ui.setText(m_nameFilter, "");
	m_ui.editBuffer(m_find).setText("");
	m_ui.setText(m_find, "");
	m_ui.setValue(m_minCalls, 0.f);
	m_ui.setValue(m_depth, 0.f);
	m_ui.setChecked(m_hideLib, false);
	m_updating = false;
	m_model.setNameFilter("");
	m_model.setMinCallCount(0);
	m_model.setMaxDepth(-1);
	m_model.setHideLibrary(false);
	m_moduleIds.clear();
	rebuildModuleCombo();
	m_infoText.clear();
	m_focusText.clear();
	m_view->clear();
	refreshLabels();
}

// ── Filters ──────────────────────────────────────────────────────────────────

void CallGraphPanel::setNameFilter(const std::string& text)
{
	if (std::string(m_ui.text(m_nameFilter)) != text)
	{
		m_ui.editBuffer(m_nameFilter).setText(text);
		m_ui.setText(m_nameFilter, text);
	}
	m_model.setNameFilter(text);
	filtersChanged();
}

void CallGraphPanel::setMinCallCount(int count)
{
	m_updating = true;
	m_ui.setValue(m_minCalls, float(count));
	m_updating = false;
	m_model.setMinCallCount(count);
	filtersChanged();
}

void CallGraphPanel::setHideLibrary(bool hide)
{
	m_updating = true;
	m_ui.setChecked(m_hideLib, hide);
	m_updating = false;
	m_model.setHideLibrary(hide);
	filtersChanged();
}

void CallGraphPanel::setModuleFilter(int moduleId)
{
	int idx = 0;
	for (size_t i = 0; i < m_moduleIds.size(); ++i)
		if (m_moduleIds[i] == moduleId) idx = int(i + 1);
	m_updating = true;
	m_ui.setValue(m_module, float(idx));
	m_updating = false;
	m_model.setModuleFilter(idx == 0 ? -1 : moduleId);
	filtersChanged();
}

void CallGraphPanel::setCallGraphDepth(int depth)
{
	const int clamped = std::clamp(depth, 0, 99);
	m_updating = true;
	m_ui.setValue(m_depth, float(clamped));
	m_updating = false;
	m_model.setMaxDepth(clamped <= 0 ? -1 : clamped);
	filtersChanged();
}

int CallGraphPanel::callGraphDepth() const
{
	return int(std::lround(m_ui.value(m_depth)));
}

void CallGraphPanel::setFocusHops(int hops)
{
	m_updating = true;
	m_ui.setValue(m_hops, float(std::clamp(hops, 1, 10)));
	m_updating = false;
}

int CallGraphPanel::focusHops() const
{
	return int(std::lround(m_ui.value(m_hops)));
}

void CallGraphPanel::focusOn(uint64_t address, int hops)
{
	if (hops > 0) setFocusHops(hops);
	hops = focusHops();
	m_model.setFocusNode(address, hops);
	m_focusText = std::format("Focus: 0x{:x} ({} hops)", address, hops);
	filtersChanged();
	m_view->fitGraph();
}

void CallGraphPanel::clearFocus()
{
	m_model.clearFocus();
	m_focusText.clear();
	filtersChanged();
}

void CallGraphPanel::setLayoutMode(model::CallGraphLayoutMode mode)
{
	m_updating = true;
	m_ui.setValue(m_layoutCombo, mode == model::CallGraphLayoutMode::Layered ? 1.f : 0.f);
	m_updating = false;
	m_model.setLayoutMode(mode);
	refresh(true);
}

void CallGraphPanel::setCollapseSccs(bool collapse)
{
	m_updating = true;
	m_ui.setChecked(m_collapse, collapse);
	m_updating = false;
	m_model.setCollapseSccs(collapse);
	refresh(true);
}

void CallGraphPanel::fitView()
{
	m_view->fitGraph();
}

void CallGraphPanel::resetZoom()
{
	m_view->resetZoom();
}

int CallGraphPanel::search(const std::string& query)
{
	const auto hits = model::graphSearchScene(m_view->scene(), query);
	m_view->setHighlighted(hits);
	if (!hits.empty())
	{
		m_view->setSelectedNode(hits.front());
		m_view->centerOn(hits.front());
	}
	return int(hits.size());
}

// ── Nodes ────────────────────────────────────────────────────────────────────

void CallGraphPanel::nodeClicked(uint64_t id)
{
	if (m_model.isSccNode(id)) return;
	const model::CallGraphNode* n = m_model.node(id);
	if (n && onFunctionActivated) onFunctionActivated(n->address, n->name);
}

std::vector<std::string> CallGraphPanel::nodeMenuItems(uint64_t id) const
{
	if (m_model.isSccNode(id)) return {"Focus (1 hop)", "Focus (2 hops)", "Focus (3 hops)", "-", "Expand SCCs"};
	return {"Focus (1 hop)", "Focus (2 hops)", "Focus (3 hops)", "-", "Navigate to Function"};
}

void CallGraphPanel::runNodeMenu(uint64_t id, int index)
{
	switch (index)
	{
	case 0:
	case 1:
	case 2: focusOn(id, index + 1); break;
	case 4:
		if (m_model.isSccNode(id))
			setCollapseSccs(false);
		else if (const model::CallGraphNode* n = m_model.node(id); n && onFunctionActivated)
			onFunctionActivated(n->address, n->name);
		break;
	default: break;
	}
}

// ── Export ───────────────────────────────────────────────────────────────────

std::string CallGraphPanel::exportDot() const
{
	return m_model.exportDot();
}

std::string CallGraphPanel::exportSvg() const
{
	model::SvgOptions o;
	o.title = "RetDec Call Graph Export";
	return model::sceneToSvg(m_view->scene(), o);
}

std::expected<void, std::string> CallGraphPanel::exportPng(const std::filesystem::path& path, float scale)
{
	return m_view->exportPng(path, scale);
}

std::expected<void, std::string> CallGraphPanel::exportTo(GraphExportFormat f, const std::filesystem::path& path)
{
	switch (f)
	{
	case GraphExportFormat::Svg: return model::writeGraphExportFile(path, exportSvg());
	case GraphExportFormat::Dot: return model::writeGraphExportFile(path, exportDot());
	case GraphExportFormat::Png: return exportPng(path);
	}
	return std::unexpected("unknown format");
}

NodeHandle CallGraphPanel::button(GraphExportFormat f) const
{
	switch (f)
	{
	case GraphExportFormat::Svg: return m_svgBtn;
	case GraphExportFormat::Png: return m_pngBtn;
	case GraphExportFormat::Dot: return m_dotBtn;
	}
	return {};
}

void CallGraphPanel::requestExport(GraphExportFormat f)
{
	if (onExportRequested)
	{
		onExportRequested(f);
		return;
	}
	if (!m_prompt) m_prompt = std::make_unique<SavePrompt>(m_ui);
	m_pendingExport = f;
	const std::string filter =
		f == GraphExportFormat::Dot ? "*.dot;*.gv" : std::format("*.{}", graphExportExtension(f));
	m_prompt->open(
		f == GraphExportFormat::Dot ? "Export Call Graph (DOT)" : "Export Call Graph",
		filter,
		[this](const std::string& path) {
			std::filesystem::path p(path);
			if (!p.has_extension()) p.replace_extension(graphExportExtension(m_pendingExport));
			(void)exportTo(m_pendingExport, p);
		});
}

} // namespace retdec::gui::ui
