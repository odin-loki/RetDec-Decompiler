/**
 * @file include/retdec/gui/model/callgraph_model.h
 * @brief Call-graph layout, filtering and focus state for the call-graph panel (no drawing).
 *
 * Port of the Qt CallGraphScene (call_graph_panel.cpp):
 *  - Tarjan SCCs with more than one member collapse into an "SCC [n]" super-node (optional here).
 *  - Force-directed spring embedding (Qt default) or a layered top-down layout.
 *  - Nodes are 34 px tall, 100..240 px wide by log2(instruction count); module colour stripe and border; library
 *    functions muted; module clusters get a translucent labelled backdrop.
 *  - Edges: width by log2(call count), blue solid for direct calls, grey dashed for indirect calls.
 *  - Filters: name (glob or substring), minimum call count (edges), hide library functions, module, maximum depth
 *    from the roots; focus mode keeps only the N-hop neighbourhood of a function.
 *  - DOT export (Qt format).
 *
 * @copyright (c) 2026 Odin Loch trading as Imortek
 */

#ifndef RETDEC_GUI_MODEL_CALLGRAPH_MODEL_H
#define RETDEC_GUI_MODEL_CALLGRAPH_MODEL_H

#include "retdec/gui/model/graph_layout.h"
#include "retdec/gui/model/graph_scene.h"
#include "retdec/gui/model/types.h"

#include <cstdint>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace retdec::gui::model {

namespace callgraph {
inline constexpr float kMinNodeW = 100.f;
inline constexpr float kMaxNodeW = 240.f;
inline constexpr float kNodeH = 34.f;
inline constexpr float kSccW = 120.f;
inline constexpr float kSccH = 40.f;
inline constexpr float kMargin = 60.f;
/// Display limit (Qt: 1000). Nodes past it are kept for DOT export and depths but not drawn.
inline constexpr int kMaxNodes = 5000;

enum NodeKind : int
{
	Function = 0,
	Scc = 1,
};
} // namespace callgraph

Rgba callGraphModuleColor(int moduleId);
float callGraphNodeWidth(int instrCount);
float callGraphEdgeWidth(int callCount);

enum class CallGraphLayoutMode : uint8_t
{
	ForceDirected, ///< Qt's spring embedding
	Layered,       ///< callers above callees (Sugiyama)
};

class CallGraphModel
{
public:
	void loadGraph(std::vector<CallGraphNode> nodes, std::vector<CallEdge> edges);
	void clearGraph();

	// ── Filters ──────────────────────────────────────────────────────────────
	void setNameFilter(std::string glob);
	void setMinCallCount(int minCount); ///< hides edges with fewer calls
	void setHideLibrary(bool hide);
	void setModuleFilter(int moduleId); ///< -1 = all modules
	void setMaxDepth(int depth);        ///< -1 = unlimited; 0 = roots only
	const std::string& nameFilter() const { return m_nameFilter; }
	int minCallCount() const { return m_minCallCount; }
	bool hideLibrary() const { return m_hideLibrary; }
	int moduleFilter() const { return m_moduleFilter; }
	int maxDepth() const { return m_maxDepth; }

	// ── Focus mode ───────────────────────────────────────────────────────────
	void setFocusNode(uint64_t address, int hops);
	void clearFocus();
	bool isFocused() const { return m_focused; }
	uint64_t focusAddress() const { return m_focusAddress; }
	int focusHops() const { return m_focusHops; }

	// ── Layout ───────────────────────────────────────────────────────────────
	void setLayoutMode(CallGraphLayoutMode mode); ///< relayouts
	CallGraphLayoutMode layoutMode() const { return m_mode; }
	void setCollapseSccs(bool collapse); ///< relayouts
	bool collapseSccs() const { return m_collapseSccs; }

	// ── Queries ──────────────────────────────────────────────────────────────
	/// Visible plain function nodes (SCC super-nodes are not counted, as in Qt).
	int visibleNodeCount() const;
	int visibleSccCount() const;
	bool isNodeVisible(uint64_t address) const;
	const CallGraphNode* node(uint64_t address) const;
	const std::vector<CallGraphNode>& nodes() const { return m_nodes; }
	const std::vector<CallEdge>& edges() const { return m_edges; }
	std::optional<int> depthOf(uint64_t address) const;
	/// Sorted distinct module ids (>= 0).
	std::vector<int> moduleIds() const;
	bool truncated() const { return int(m_nodes.size()) > callgraph::kMaxNodes; }
	/// Collapsed SCCs: super-node id (representative address) -> member addresses.
	const std::unordered_map<uint64_t, std::vector<uint64_t>>& sccGroups() const { return m_sccMembers; }
	bool isSccNode(uint64_t sceneId) const { return m_sccMembers.count(sceneId) > 0; }
	/// The scene node showing `address` (itself, or its SCC super-node), if drawn.
	std::optional<uint64_t> sceneNodeFor(uint64_t address) const;
	int callerCount(uint64_t address) const;
	int calleeCount(uint64_t address) const;

	const GraphScene& scene() const { return m_scene; }

	/// Graphviz DOT of every node and (deduplicated) edge, Qt format.
	std::string exportDot() const;

private:
	void relayout();
	void recomputeDepths();
	void applyFilters();

	std::vector<CallGraphNode> m_nodes;
	std::vector<CallEdge> m_edges;
	std::unordered_map<uint64_t, size_t> m_byAddr;
	GraphAdjacency m_adj;
	std::unordered_map<uint64_t, int> m_depth;

	std::string m_nameFilter;
	int m_minCallCount = 0;
	bool m_hideLibrary = false;
	int m_moduleFilter = -1;
	int m_maxDepth = -1;

	bool m_focused = false;
	uint64_t m_focusAddress = 0;
	int m_focusHops = 2;
	std::unordered_set<uint64_t> m_focusSet;

	CallGraphLayoutMode m_mode = CallGraphLayoutMode::ForceDirected;
	bool m_collapseSccs = true;

	GraphScene m_scene;
	std::unordered_map<uint64_t, std::vector<uint64_t>> m_sccMembers; ///< rep -> members
	std::unordered_map<uint64_t, uint64_t> m_repOf;                   ///< member -> rep
	std::vector<int> m_edgeCalls;                                     ///< per scene edge: aggregated call count
	std::unordered_set<uint64_t> m_drawn;                             ///< plain nodes drawn (not truncated / SCC)
};

} // namespace retdec::gui::model

#endif // RETDEC_GUI_MODEL_CALLGRAPH_MODEL_H
