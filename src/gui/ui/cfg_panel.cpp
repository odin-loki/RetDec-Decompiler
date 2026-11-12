/**
 * @file src/gui/ui/cfg_panel.cpp
 * @brief FuseUI control-flow-graph panel.
 * @copyright (c) 2026 Odin Loch trading as Imortek
 */

#include "retdec/gui/ui/cfg_panel.h"

#include "retdec/gui/model/graph_export.h"
#include "retdec/gui/model/graph_search.h"

#include <fuse/ui/widgets/widgets.hpp>

#include <format>

namespace retdec::gui::ui {

namespace w = fuse::ui::widgets;
using fuse::ui::NodeHandle;
using fuse::ui::SignalArgs;
using fuse::ui::SignalKind;
using fuse::ui::UiContext;


CfgPanel::CfgPanel(UiContext& ui, NodeHandle parent): m_ui(ui)
{
	m_root = w::panel(ui, parent);
	ui.setCss(m_root, "flex-grow", "1");
	ui.setCss(m_root, "gap", "0px");
	ui.setCss(m_root, "padding", "0px");
	ui.setCss(m_root, "background", "#1e1e2e");
	ui.setA11y(m_root, fuse::ui::A11yRole::Group, kTitle);

	// ── Toolbar ──────────────────────────────────────────────────────────────
	m_toolbar = graphToolbarRow(ui, m_root);
	m_funcLabel = graphMutedLabel(ui, m_toolbar, "No function selected");
	ui.setCss(m_funcLabel, "flex-grow", "1");
	m_infoLabel = graphMutedLabel(ui, m_toolbar, "");
	m_search = w::lineEdit(ui, m_toolbar, "");
	ui.setCss(m_search, "width", "150px");
	ui.setCss(m_search, "font-size", "12px");
	ui.setCss(m_search, "padding", "2px 6px");
	ui.setA11y(m_search, fuse::ui::A11yRole::TextField, "Search blocks");
	w::setPlaceholderText(ui, m_search, "Search blocks\u2026");
	m_matchLabel = graphMutedLabel(ui, m_toolbar, "");
	m_fitBtn = graphToolButton(ui, m_toolbar, "Fit");
	m_resetBtn = graphToolButton(ui, m_toolbar, "100%");
	m_chainsBtn = graphToolButton(ui, m_toolbar, "Expand chains");
	w::setTooltip(ui, m_chainsBtn, "Show the full control-flow graph with compressed instruction chains expanded");
	m_minimapCheck = w::checkBox(ui, m_toolbar, "Minimap", true);
	ui.setCss(m_minimapCheck, "font-size", "12px");
	m_svgBtn = graphToolButton(ui, m_toolbar, "Export SVG");
	m_pngBtn = graphToolButton(ui, m_toolbar, "Export PNG");
	m_dotBtn = graphToolButton(ui, m_toolbar, "Export DOT");

	// ── Body: empty state | graph ────────────────────────────────────────────
	m_body = w::panel(ui, m_root);
	ui.setCss(m_body, "flex-grow", "1");
	ui.setCss(m_body, "padding", "0px");
	m_empty = w::panel(ui, m_body);
	ui.setCss(m_empty, "flex-grow", "1");
	ui.setCss(m_empty, "align-items", "center");
	ui.setCss(m_empty, "justify-content", "center");
	ui.setCss(m_empty, "gap", "6px");
	const NodeHandle title = w::label(ui, m_empty, "No control-flow graph");
	ui.setCss(title, "font-size", "16px");
	ui.setCss(title, "color", "#cdd6f4");
	graphMutedLabel(ui, m_empty, "Select a function to visualize its basic blocks and edges.");
	m_view = std::make_unique<GraphView>(ui, m_body);

	// ── Wiring ───────────────────────────────────────────────────────────────
	auto onClick = [&](NodeHandle n, auto fn) {
		ui.connect(n, SignalKind::Clicked, fn);
	};
	onClick(m_fitBtn, [this](UiContext&, NodeHandle, const SignalArgs&) { fitView(); });
	onClick(m_resetBtn, [this](UiContext&, NodeHandle, const SignalArgs&) { resetZoom(); });
	onClick(m_chainsBtn, [this](UiContext&, NodeHandle, const SignalArgs&) { toggleChains(); });
	onClick(m_svgBtn, [this](UiContext&, NodeHandle, const SignalArgs&) { requestExport(GraphExportFormat::Svg); });
	onClick(m_pngBtn, [this](UiContext&, NodeHandle, const SignalArgs&) { requestExport(GraphExportFormat::Png); });
	onClick(m_dotBtn, [this](UiContext&, NodeHandle, const SignalArgs&) { requestExport(GraphExportFormat::Dot); });
	ui.connect(m_minimapCheck, SignalKind::Toggled,
		[this](UiContext&, NodeHandle, const SignalArgs& a) { m_view->setMinimapVisible(a.flag); });
	ui.connect(m_search, SignalKind::TextChanged,
		[this](UiContext&, NodeHandle, const SignalArgs& a) { search(std::string(a.text)); });
	ui.connect(m_search, SignalKind::Submitted, [this](UiContext&, NodeHandle, const SignalArgs&) { nextMatch(); });

	m_view->onNodeClicked = [this](uint64_t id) { blockClicked(id); };
	m_view->onNodeContextMenu = [this](uint64_t id, fuse::ui::Vec2 at) {
		std::string items;
		for (const auto& it: blockMenuItems(id))
			items += (items.empty() ? "" : "\n") + it;
		m_view->openMenu(items, at, [this, id](int index) { runBlockMenu(id, index); });
	};
	updateEmptyState();
}

CfgPanel::~CfgPanel()
{
	m_view.reset();
	if (m_prompt && m_ui.alive(m_prompt->dialog())) m_ui.destroy(m_prompt->dialog());
	m_prompt.reset();
	if (m_ui.alive(m_root)) m_ui.destroy(m_root);
}

// ── Data ─────────────────────────────────────────────────────────────────────

void CfgPanel::setCfg(std::vector<model::BasicBlockData> blocks, std::vector<model::CFGEdgeData> edges)
{
	m_blocks = std::move(blocks);
	m_edges = std::move(edges);
	m_forceUncompressed = false;
	relayout(true);
}

void CfgPanel::relayout(bool fit)
{
	model::CfgLayoutOptions opt;
	opt.forceUncompressed = m_forceUncompressed;
	m_layout = model::layoutCfg(m_blocks, m_edges, opt);
	m_view->setScene(m_layout.scene, fit);
	m_matches.clear();
	if (const std::string q(m_ui.text(m_search)); !q.empty()) search(q);
	refreshLabels();
	updateEmptyState();
}

void CfgPanel::setFunction(uint64_t address, const std::string& name)
{
	m_funcAddress = address;
	m_funcName = name;
	refreshLabels();
}

void CfgPanel::clear()
{
	m_blocks.clear();
	m_edges.clear();
	m_layout = {};
	m_forceUncompressed = false;
	m_funcAddress = 0;
	m_funcName.clear();
	m_matches.clear();
	m_view->clear();
	refreshLabels();
	updateEmptyState();
}

void CfgPanel::refreshLabels()
{
	m_ui.setText(m_funcLabel, functionLabel());
	m_ui.setText(m_infoLabel, infoLabel());
	m_ui.setText(m_chainsBtn, expandChainsLabel());
	m_ui.setText(m_matchLabel,
		m_matches.empty() ? std::string() : std::format("{}/{}", m_matchIndex + 1, m_matches.size()));
}

std::string CfgPanel::functionLabel() const
{
	if (m_funcAddress == 0 && m_funcName.empty()) return "No function selected";
	return std::format("{}  0x{:x}", m_funcName, m_funcAddress);
}

std::string CfgPanel::infoLabel() const
{
	if (m_blocks.empty()) return {};
	std::string s = std::format("{} blocks", m_blocks.size());
	if (m_layout.compressed) s += " (compressed)";
	return s;
}

std::string CfgPanel::expandChainsLabel() const
{
	const bool large = int(m_blocks.size()) > model::cfg::kCompressThreshold;
	return (m_forceUncompressed && large && !m_layout.compressed) ? "Compress chains" : "Expand chains";
}

void CfgPanel::updateEmptyState()
{
	const bool empty = m_layout.scene.empty();
	graphSetShown(m_ui, m_empty, empty);
	graphSetShown(m_ui, m_view->node(), !empty);
}

bool CfgPanel::emptyStateVisible() const
{
	return m_layout.scene.empty();
}

// ── Commands ─────────────────────────────────────────────────────────────────

void CfgPanel::fitView()
{
	m_view->fitGraph();
}

void CfgPanel::resetZoom()
{
	m_view->resetZoom();
}

void CfgPanel::toggleChains()
{
	if (m_blocks.empty()) return;
	const bool large = int(m_blocks.size()) > model::cfg::kCompressThreshold;
	setChainsExpanded(!(m_forceUncompressed && large));
}

void CfgPanel::setChainsExpanded(bool expanded)
{
	m_forceUncompressed = expanded;
	relayout(true);
}

void CfgPanel::setMinimapVisible(bool on)
{
	m_ui.setChecked(m_minimapCheck, on);
	m_view->setMinimapVisible(on);
}

int CfgPanel::search(const std::string& query)
{
	m_matches = model::graphSearchScene(m_layout.scene, query);
	m_matchIndex = 0;
	m_view->setHighlighted(m_matches);
	if (!m_matches.empty())
	{
		m_view->setSelectedNode(m_matches.front());
		m_view->centerOn(m_matches.front());
	}
	refreshLabels();
	return int(m_matches.size());
}

std::optional<uint64_t> CfgPanel::nextMatch()
{
	if (m_matches.empty()) return std::nullopt;
	m_matchIndex = (m_matchIndex + 1) % m_matches.size();
	const uint64_t id = m_matches[m_matchIndex];
	m_view->setSelectedNode(id);
	m_view->centerOn(id);
	refreshLabels();
	return id;
}

// ── Blocks ───────────────────────────────────────────────────────────────────

void CfgPanel::blockClicked(uint64_t id)
{
	const auto it = m_layout.addressOf.find(id);
	const bool chain = [&] {
		const model::SceneNode* n = m_layout.scene.node(id);
		return n && n->kind == model::cfg::Chain;
	}();
	if (it != m_layout.addressOf.end() && onBlockActivated) onBlockActivated(it->second);
	// Clicking a compressed chain expands the graph (Qt: queued after the click).
	if (chain)
		m_ui.post([](UiContext& ui, NodeHandle n, uint64_t self, uint64_t) {
			if (ui.alive(n)) reinterpret_cast<CfgPanel*>(self)->setChainsExpanded(true);
		}, m_root, reinterpret_cast<uint64_t>(this));
}

std::vector<std::string> CfgPanel::blockMenuItems(uint64_t blockId) const
{
	std::vector<std::string> items{"Copy Address", "Go to Function"};
	if (const model::SceneNode* n = m_layout.scene.node(blockId); n && n->kind == model::cfg::Chain)
		items.push_back("Expand chain");
	return items;
}

void CfgPanel::runBlockMenu(uint64_t blockId, int index)
{
	switch (index)
	{
	case 0:
		if (const auto it = m_layout.addressOf.find(blockId); it != m_layout.addressOf.end())
			m_ui.setClipboardText(model::hexAddress(it->second));
		break;
	case 1:
		if (m_funcAddress != 0 && onAddressNavigated) onAddressNavigated(m_funcAddress);
		break;
	case 2: setChainsExpanded(true); break;
	default: break;
	}
}

// ── Export ───────────────────────────────────────────────────────────────────

std::string CfgPanel::exportSvg() const
{
	model::SvgOptions o;
	o.title = "RetDec CFG Export";
	return model::sceneToSvg(m_layout.scene, o);
}

std::string CfgPanel::exportDot() const
{
	return model::cfgToDot(m_blocks, m_edges, m_funcName.empty() ? "cfg" : m_funcName);
}

std::expected<void, std::string> CfgPanel::exportPng(const std::filesystem::path& path, float scale)
{
	return m_view->exportPng(path, scale);
}

std::expected<void, std::string> CfgPanel::exportTo(GraphExportFormat f, const std::filesystem::path& path)
{
	switch (f)
	{
	case GraphExportFormat::Svg: return model::writeGraphExportFile(path, exportSvg());
	case GraphExportFormat::Dot: return model::writeGraphExportFile(path, exportDot());
	case GraphExportFormat::Png: return exportPng(path);
	}
	return std::unexpected("unknown format");
}

fuse::ui::NodeHandle CfgPanel::button(GraphExportFormat f) const
{
	switch (f)
	{
	case GraphExportFormat::Svg: return m_svgBtn;
	case GraphExportFormat::Png: return m_pngBtn;
	case GraphExportFormat::Dot: return m_dotBtn;
	}
	return {};
}

void CfgPanel::requestExport(GraphExportFormat f)
{
	if (onExportRequested)
	{
		onExportRequested(f);
		return;
	}
	if (m_layout.scene.empty()) return;
	if (!m_prompt) m_prompt = std::make_unique<SavePrompt>(m_ui);
	m_pendingExport = f;
	const std::string filter = std::format("*.{}", graphExportExtension(f));
	m_prompt->open("Export CFG", filter, [this](const std::string& path) {
		std::filesystem::path p(path);
		if (!p.has_extension()) p.replace_extension(graphExportExtension(m_pendingExport));
		(void)exportTo(m_pendingExport, p);
	});
}

} // namespace retdec::gui::ui
