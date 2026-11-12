/**
 * @file include/retdec/gui/ui/call_graph_panel.h
 * @brief Zoomable, filterable call-graph panel (FuseUI port of the Qt CallGraphPanel).
 *
 * Filter row: name filter (glob, or substring without wildcards), Min calls, Depth (0 = all), Hide lib, module combo,
 * Focus hops, Clear Focus. View row: layout (Force-directed / Layered), Collapse SCCs, focus and info labels, Fit, 100%,
 * Export DOT / SVG / PNG. Body: GraphView with minimap.
 *
 * Nodes: rounded boxes sized by instruction count with a module colour stripe (library functions muted), SCC
 * super-nodes (dashed peach "SCC [n]"), module cluster backdrops; edges weighted by call count, dashed for indirect
 * calls. Click a function: onFunctionActivated. Right-click: Focus (1/2/3 hops) / Navigate to Function. Double-click
 * an SCC super-node: expand SCCs.
 *
 * @copyright (c) 2026 Odin Loch trading as Imortek
 */

#ifndef RETDEC_GUI_UI_CALL_GRAPH_PANEL_H
#define RETDEC_GUI_UI_CALL_GRAPH_PANEL_H

#include "retdec/gui/model/callgraph_model.h"
#include "retdec/gui/model/types.h"
#include "retdec/gui/ui/graph_view.h"

#include <fuse/ui/ui.hpp>

#include <cstdint>
#include <expected>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace retdec::gui::ui {

class CallGraphPanel
{
public:
	CallGraphPanel(fuse::ui::UiContext& ui, fuse::ui::NodeHandle parent);
	~CallGraphPanel();
	CallGraphPanel(const CallGraphPanel&) = delete;
	CallGraphPanel& operator=(const CallGraphPanel&) = delete;

	fuse::ui::NodeHandle root() const { return m_root; }
	static constexpr const char* kTitle = "Call Graph";

	// ── Data in ──────────────────────────────────────────────────────────────
	/// Loads or replaces the graph (Qt loadGraph): rebuilds the module list (keeping the selected module when it still
	/// exists), lays out, fits.
	void setCallGraph(std::vector<model::CallGraphNode> nodes, std::vector<model::CallEdge> edges);
	/// Legacy: function names only; addresses are the indices (Qt setCallGraph(QStringList, edges)).
	void setCallGraph(const std::vector<std::string>& functionNames, std::vector<model::CallEdge> edges);
	/// The current function (Qt onFunctionSelected): shown in the focus label, selected and centred when drawn.
	void setFunction(uint64_t address, const std::string& name);
	/// Clears the graph and resets every filter (Qt clear).
	void clear();

	// ── Filters / focus / layout (also driven by the toolbar) ────────────────
	void setNameFilter(const std::string& text);
	void setMinCallCount(int count);
	void setHideLibrary(bool hide);
	/// -1 = all modules.
	void setModuleFilter(int moduleId);
	/// 0 = all (Qt spin box "All"), else the maximum depth from the roots.
	void setCallGraphDepth(int depth);
	int callGraphDepth() const;
	/// Shows only the `hops`-neighbourhood of `address` (hops < 0: the Focus hops spin box value).
	void focusOn(uint64_t address, int hops = -1);
	void clearFocus();
	void setFocusHops(int hops);
	int focusHops() const;
	void setLayoutMode(model::CallGraphLayoutMode mode);
	void setCollapseSccs(bool collapse);
	void fitView();
	void resetZoom();
	/// Highlights matching visible nodes, selects and centres the first; returns the match count.
	int search(const std::string& query);

	// ── Export ───────────────────────────────────────────────────────────────
	std::string exportDot() const;
	std::string exportSvg() const;
	std::expected<void, std::string> exportPng(const std::filesystem::path& path, float scale = 2.f);
	std::expected<void, std::string> exportTo(GraphExportFormat format, const std::filesystem::path& path);

	// ── State (tests, main window) ───────────────────────────────────────────
	const model::CallGraphModel& graph() const { return m_model; }
	GraphView& view() { return *m_view; }
	int visibleNodeCount() const { return m_model.visibleNodeCount(); }
	std::string infoLabel() const;
	std::string focusLabel() const;
	bool clearFocusEnabled() const;
	/// Module combo items ("All modules", "Module N", ...).
	std::vector<std::string> moduleItems() const;
	fuse::ui::NodeHandle nameFilterBox() const { return m_nameFilter; }
	fuse::ui::NodeHandle minCallsSpin() const { return m_minCalls; }
	fuse::ui::NodeHandle depthSpin() const { return m_depth; }
	fuse::ui::NodeHandle hideLibraryCheck() const { return m_hideLib; }
	fuse::ui::NodeHandle moduleCombo() const { return m_module; }
	fuse::ui::NodeHandle hopsSpin() const { return m_hops; }
	fuse::ui::NodeHandle clearFocusButton() const { return m_clearFocus; }
	fuse::ui::NodeHandle layoutCombo() const { return m_layoutCombo; }
	fuse::ui::NodeHandle collapseSccsCheck() const { return m_collapse; }
	fuse::ui::NodeHandle fitButton() const { return m_fitBtn; }
	fuse::ui::NodeHandle resetButton() const { return m_resetBtn; }
	fuse::ui::NodeHandle button(GraphExportFormat f) const;

	/// Context-menu lines for a node ("-" = separator), and running line `index` (as picking it does).
	std::vector<std::string> nodeMenuItems(uint64_t sceneId) const;
	void runNodeMenu(uint64_t sceneId, int index);

	// ── Events out ───────────────────────────────────────────────────────────
	/// A function node was clicked or "Navigate to Function" chosen (Qt functionNavigationRequested).
	std::function<void(uint64_t address, const std::string& name)> onFunctionActivated;
	/// Export button pressed; when set the host chooses the path and calls exportTo(), else a save dialog opens.
	std::function<void(GraphExportFormat format)> onExportRequested;

private:
	void refresh(bool fit);
	void filtersChanged();
	void rebuildModuleCombo();
	void refreshLabels();
	void nodeClicked(uint64_t id);
	void requestExport(GraphExportFormat f);

	fuse::ui::UiContext& m_ui;
	fuse::ui::NodeHandle m_root;
	fuse::ui::NodeHandle m_nameFilter, m_minCalls, m_depth, m_hideLib, m_module, m_hops, m_clearFocus;
	fuse::ui::NodeHandle m_layoutCombo, m_collapse, m_minimapCheck, m_focusLabel, m_infoLabel;
	fuse::ui::NodeHandle m_fitBtn, m_resetBtn, m_dotBtn, m_svgBtn, m_pngBtn;
	std::unique_ptr<GraphView> m_view;
	std::unique_ptr<SavePrompt> m_prompt;
	GraphExportFormat m_pendingExport = GraphExportFormat::Dot;

	model::CallGraphModel m_model;
	std::vector<int> m_moduleIds; ///< combo index i + 1 -> module id
	std::string m_focusText, m_infoText;
	bool m_updating = false; ///< programmatic widget updates in progress
};

} // namespace retdec::gui::ui

#endif // RETDEC_GUI_UI_CALL_GRAPH_PANEL_H
