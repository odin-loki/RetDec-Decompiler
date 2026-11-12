/**
 * @file include/retdec/gui/model/cfg_graph.h
 * @brief Control-flow-graph layout for the CFG panel: chain compression, loop detection, Sugiyama layout, edge
 *        routing, the drawable scene and DOT export.
 *
 * Port of the Qt CFGScene (cfg_panel.cpp) without drawing:
 *  - Blocks are 230 px wide rounded boxes: address header + up to 3 instruction previews + "… N more".
 *  - Graphs with more than 200 blocks are compressed: non-branching chains (in = out = 1, not entry / exit / loop
 *    header) collapse into one "[chain: N instructions]" node; forceUncompressed expands them.
 *  - Back edges (DFS) and EdgeKind::BackEdge are drawn as dashed orange arcs to the left (from the source's left side
 *    into the target's header, so they never run under a block).
 *  - Blocks sharing a loopId get a translucent coloured loop region; when the data has no loop ids, natural loops
 *    are detected from the back edges (optional).
 *
 * @copyright (c) 2026 Odin Loch trading as Imortek
 */

#ifndef RETDEC_GUI_MODEL_CFG_GRAPH_H
#define RETDEC_GUI_MODEL_CFG_GRAPH_H

#include "retdec/gui/model/graph_layout.h"
#include "retdec/gui/model/graph_scene.h"
#include "retdec/gui/model/types.h"

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace retdec::gui::model {

namespace cfg {
inline constexpr float kBlockWidth = 230.f;
inline constexpr float kHeaderH = 24.f;
inline constexpr float kInstrH = 16.f;
inline constexpr int kMaxPreview = 3;
inline constexpr float kPadX = 10.f;
inline constexpr float kRadius = 7.f;
inline constexpr float kFooterH = 4.f;
inline constexpr int kCompressThreshold = 200;
inline constexpr float kNodeSpacingH = 270.f; ///< Qt: left edge to left edge on a level
inline constexpr float kLevelH = 140.f;       ///< Qt: top to top of consecutive levels
inline constexpr float kMargin = 40.f;
inline constexpr float kBackEdgeArc = 80.f; ///< how far left of the blocks a back edge arcs
inline constexpr float kTextSize = 11.f;    ///< Qt: 8 pt monospace

/// Scene node kinds.
enum NodeKind : int
{
	Block = 0,
	Chain = 1, ///< compressed chain summary
};
} // namespace cfg

/// Edge colour per kind (Catppuccin: grey, green, red, peach, mauve, red).
Rgba cfgEdgeColor(EdgeKind kind);
/// Back edges and unresolved indirect jumps are dashed.
bool cfgEdgeDashed(EdgeKind kind);
const char* edgeKindName(EdgeKind kind);

/// Header + previews (+ "more" line) + footer.
float cfgBlockHeight(const BasicBlockData& block, int maxPreview = cfg::kMaxPreview);
/// "0x%016x" plus "  [entry]" / "  [exit]" / "  [loop header]".
std::string cfgBlockHeaderText(const BasicBlockData& block);
std::string cfgBlockTooltip(const BasicBlockData& block, int maxLines = 24);
/// True for a compressed chain pseudo-block.
bool isChainSummary(const BasicBlockData& block);

struct CfgGraphData
{
	std::vector<BasicBlockData> blocks;
	std::vector<CFGEdgeData> edges;
};

/// Collapses non-branching chains (Qt CFGScene::compressChains). `members` (optional) receives, per kept head id,
/// the ids folded into it (head first).
CfgGraphData compressChains(const std::vector<BasicBlockData>& blocks, const std::vector<CFGEdgeData>& edges,
	std::unordered_map<uint64_t, std::vector<uint64_t>>* members = nullptr);

/// Entry block: the first isEntry block, else the first block without predecessors, else the first block.
uint64_t cfgEntryBlock(const std::vector<BasicBlockData>& blocks, const std::vector<CFGEdgeData>& edges);

struct NaturalLoop
{
	uint64_t header = 0;
	std::vector<uint64_t> body; ///< includes the header, sorted
	std::vector<std::pair<uint64_t, uint64_t>> backEdges;
};
/// Natural loops of the DFS back edges from the entry (loops sharing a header are merged), ordered by header
/// position in `blocks`.
std::vector<NaturalLoop> detectNaturalLoops(const std::vector<BasicBlockData>& blocks, const std::vector<CFGEdgeData>& edges);
/// When no block has a loop id, marks headers and assigns loop ids from detectNaturalLoops (innermost loop wins).
/// Returns the number of loops assigned (0 when the data already had loop information).
int annotateLoops(std::vector<BasicBlockData>& blocks, const std::vector<CFGEdgeData>& edges);

struct CfgLayoutOptions
{
	bool forceUncompressed = false;
	int compressThreshold = cfg::kCompressThreshold;
	bool autoDetectLoops = true;
	bool showLoopRegions = true;
	int maxPreview = cfg::kMaxPreview;
};

struct CfgLayout
{
	GraphScene scene;
	bool compressed = false;
	int nodeCount = 0; ///< blocks drawn (after compression)
	EdgeSet backEdges;
	uint64_t entry = 0;
	std::unordered_map<uint64_t, uint64_t> addressOf; ///< drawn block id -> start address
	std::unordered_map<uint64_t, std::vector<uint64_t>> chainMembers;
	std::vector<NaturalLoop> loops;
};

/// Full CFG layout: compression, back edges, layered layout, edge routing (ports spread along block edges, back
/// edges arcing left), loop regions, and the drawable scene.
CfgLayout layoutCfg(const std::vector<BasicBlockData>& blocks, const std::vector<CFGEdgeData>& edges,
	const CfgLayoutOptions& options = {});

/// Graphviz DOT of a CFG (record-like boxes with the instructions, edges coloured / styled by kind).
std::string cfgToDot(const std::vector<BasicBlockData>& blocks, const std::vector<CFGEdgeData>& edges,
	const std::string& title = "cfg");

} // namespace retdec::gui::model

#endif // RETDEC_GUI_MODEL_CFG_GRAPH_H
