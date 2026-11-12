/**
 * @file include/retdec/gui/ui/cfg_panel.h
 * @brief Interactive control-flow-graph panel (FuseUI port of the Qt CFGPanel).
 *
 * Toolbar: function label, block count ("N blocks (compressed)"), search box, Fit, 100%, Expand / Compress chains,
 * Minimap toggle, Export SVG / PNG / DOT. Body: an empty state ("No control-flow graph") or the graph canvas
 * (GraphView) with the minimap in its top-right corner.
 *
 * Blocks: 230 px rounded boxes with the address header ([entry] green, [exit] red, [loop header] blue) and up to three
 * instruction previews; edges coloured per EdgeKind (dashed orange back-edge arcs, dashed red unresolved jumps); loop
 * bodies get translucent regions. Graphs above 200 blocks compress non-branching chains; clicking a chain node (or
 * "Expand chains") shows the full graph.
 *
 * Events: clicking a block reports its start address (Qt blockNavigationRequested); the block context menu offers
 * Copy Address, Go to Function (the function set with setFunction) and Expand chain.
 *
 * @copyright (c) 2026 Odin Loch trading as Imortek
 */

#ifndef RETDEC_GUI_UI_CFG_PANEL_H
#define RETDEC_GUI_UI_CFG_PANEL_H

#include "retdec/gui/model/cfg_graph.h"
#include "retdec/gui/model/types.h"
#include "retdec/gui/ui/graph_view.h"

#include <fuse/ui/ui.hpp>

#include <cstdint>
#include <expected>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace retdec::gui::ui {

class CfgPanel
{
public:
	CfgPanel(fuse::ui::UiContext& ui, fuse::ui::NodeHandle parent);
	~CfgPanel();
	CfgPanel(const CfgPanel&) = delete;
	CfgPanel& operator=(const CfgPanel&) = delete;

	fuse::ui::NodeHandle root() const { return m_root; }
	static constexpr const char* kTitle = "Control Flow Graph";

	// ── Data in ──────────────────────────────────────────────────────────────
	/// Loads or replaces the displayed CFG (Qt loadCFG); fits the view.
	void setCfg(std::vector<model::BasicBlockData> blocks, std::vector<model::CFGEdgeData> edges);
	/// The function the CFG belongs to (Qt onFunctionSelected): label "name  0xaddr", target of Go to Function.
	void setFunction(uint64_t address, const std::string& name);
	void clear();

	// ── Commands (toolbar / shortcuts) ───────────────────────────────────────
	void fitView();
	void resetZoom();
	/// Expand chains (or, when expanded and large, compress again) — the toolbar button.
	void toggleChains();
	void setChainsExpanded(bool expanded);
	bool chainsExpanded() const { return m_forceUncompressed; }
	void setMinimapVisible(bool on);
	/// Highlights matching blocks (address or instruction text; glob or substring), selects and centres the first.
	/// Returns the number of matches. Empty clears.
	int search(const std::string& query);
	/// Selects and centres the next match (wraps). Returns its block id.
	std::optional<uint64_t> nextMatch();

	// ── Export ───────────────────────────────────────────────────────────────
	std::string exportSvg() const;
	std::string exportDot() const;
	std::expected<void, std::string> exportPng(const std::filesystem::path& path, float scale = 2.f);
	/// Writes `format` to `path`.
	std::expected<void, std::string> exportTo(GraphExportFormat format, const std::filesystem::path& path);

	// ── State (tests, main window) ───────────────────────────────────────────
	bool isCompressed() const { return m_layout.compressed; }
	int nodeCount() const { return m_layout.nodeCount; } ///< blocks drawn
	int blockCount() const { return int(m_blocks.size()); }
	bool emptyStateVisible() const;
	std::string functionLabel() const;
	std::string infoLabel() const;
	std::string expandChainsLabel() const;
	uint64_t functionAddress() const { return m_funcAddress; }
	const model::CfgLayout& layout() const { return m_layout; }
	GraphView& view() { return *m_view; }
	fuse::ui::NodeHandle searchBox() const { return m_search; }
	fuse::ui::NodeHandle button(GraphExportFormat f) const;
	fuse::ui::NodeHandle fitButton() const { return m_fitBtn; }
	fuse::ui::NodeHandle resetButton() const { return m_resetBtn; }
	fuse::ui::NodeHandle expandChainsButton() const { return m_chainsBtn; }

	// ── Events out ───────────────────────────────────────────────────────────
	/// A block was clicked: its start address (Qt blockNavigationRequested).
	std::function<void(uint64_t address)> onBlockActivated;
	/// "Go to Function" from a block's context menu: the current function's address (Qt addressNavigated).
	std::function<void(uint64_t address)> onAddressNavigated;
	/// An Export button was pressed. When set, the host picks the path and calls exportTo(); when unset the panel
	/// opens FuseUI's save dialog itself.
	std::function<void(GraphExportFormat format)> onExportRequested;

	/// Context-menu items for a block (Copy Address, Go to Function, Expand chain) — what right-click opens.
	std::vector<std::string> blockMenuItems(uint64_t blockId) const;
	/// Runs context-menu item `index` for `blockId` (as picking it in the menu does).
	void runBlockMenu(uint64_t blockId, int index);

private:
	void relayout(bool fit);
	void refreshLabels();
	void updateEmptyState();
	void requestExport(GraphExportFormat f);
	void blockClicked(uint64_t id);

	fuse::ui::UiContext& m_ui;
	fuse::ui::NodeHandle m_root, m_toolbar, m_funcLabel, m_infoLabel, m_search, m_matchLabel;
	fuse::ui::NodeHandle m_fitBtn, m_resetBtn, m_chainsBtn, m_minimapCheck, m_svgBtn, m_pngBtn, m_dotBtn;
	fuse::ui::NodeHandle m_body, m_empty;
	GraphExportFormat m_pendingExport = GraphExportFormat::Svg;
	std::unique_ptr<GraphView> m_view;
	std::unique_ptr<SavePrompt> m_prompt;

	std::vector<model::BasicBlockData> m_blocks;
	std::vector<model::CFGEdgeData> m_edges;
	model::CfgLayout m_layout;
	bool m_forceUncompressed = false;
	uint64_t m_funcAddress = 0;
	std::string m_funcName;
	std::vector<uint64_t> m_matches;
	size_t m_matchIndex = 0;
};

} // namespace retdec::gui::ui

#endif // RETDEC_GUI_UI_CFG_PANEL_H
