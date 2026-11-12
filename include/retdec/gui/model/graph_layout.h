/**
 * @file include/retdec/gui/model/graph_layout.h
 * @brief Graph algorithms behind the CFG and call-graph views.
 *
 *  - Back-edge identification (iterative DFS), longest-path ranking, barycentric crossing minimisation and
 *    neighbour-aligned coordinate assignment: a layered (Sugiyama) layout.
 *  - Tarjan strongly connected components (iterative).
 *  - Breadth-first depth from the roots and depth-limited neighbourhoods (focus mode).
 *  - Fruchterman-Reingold force-directed layout (grid-accelerated repulsion for large graphs).
 *
 * Node ids are uint64_t (block ids, function addresses). Deterministic: same input, same output.
 *
 * @copyright (c) 2026 Odin Loch trading as Imortek
 */

#ifndef RETDEC_GUI_MODEL_GRAPH_LAYOUT_H
#define RETDEC_GUI_MODEL_GRAPH_LAYOUT_H

#include "retdec/gui/model/graph_scene.h"

#include <cstdint>
#include <optional>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace retdec::gui::model {

struct EdgeRef
{
	uint64_t from = 0;
	uint64_t to = 0;
};

struct EdgePairHash
{
	size_t operator()(const std::pair<uint64_t, uint64_t>& p) const noexcept
	{
		return std::hash<uint64_t>{}(p.first * 0x9E3779B97F4A7C15ull ^ (p.second + 0x632BE59BD9B4E019ull));
	}
};
using EdgeSet = std::unordered_set<std::pair<uint64_t, uint64_t>, EdgePairHash>;
using Adjacency = std::unordered_map<uint64_t, std::vector<uint64_t>>;

struct GraphAdjacency
{
	Adjacency succ;
	Adjacency pred;
	static GraphAdjacency build(const std::vector<EdgeRef>& edges);
	const std::vector<uint64_t>& successors(uint64_t n) const;
	const std::vector<uint64_t>& predecessors(uint64_t n) const;
};

// ── Layered layout ───────────────────────────────────────────────────────────

/// Edges closing a cycle in a DFS from `entry` first, then from every node not yet reached (in `nodes` order).
/// Removing them leaves the graph acyclic.
EdgeSet findBackEdges(const std::vector<uint64_t>& nodes, const Adjacency& succ, std::optional<uint64_t> entry);

/// Longest-path layering ignoring `backEdges`: sources get rank 0, every other node max(rank(pred)) + 1.
std::unordered_map<uint64_t, int>
longestPathRanks(const std::vector<uint64_t>& nodes, const Adjacency& succ, const EdgeSet& backEdges);

/// Barycentric crossing minimisation: `sweeps` rounds of a top-down (by predecessors) then a bottom-up (by
/// successors) pass, each a stable sort of a level by the mean position of its neighbours on the adjacent level.
void barycentricSweep(
	std::vector<std::vector<uint64_t>>& levels, const Adjacency& pred, const Adjacency& succ, int sweeps = 2);

/// Number of crossings between consecutive levels (edges spanning more than one level are ignored).
int countCrossings(const std::vector<std::vector<uint64_t>>& levels, const Adjacency& succ);

struct LayeredOptions
{
	float nodeGap = 40.f; ///< horizontal gap between neighbours on a level
	float rankGap = 48.f; ///< vertical gap between the tallest node of a level and the next level
	float margin = 40.f;  ///< top-left margin of the result
	int sweeps = 2;
	int alignPasses = 4; ///< neighbour-alignment passes (0 = packed and centred levels only)
};

struct LayeredLayout
{
	std::unordered_map<uint64_t, GPoint> topLeft;
	std::unordered_map<uint64_t, int> rank;
	std::vector<std::vector<uint64_t>> levels; ///< node ids per rank, in drawing order
	EdgeSet backEdges;
	GRect bounds; ///< of the node boxes (margin not included)
};

/// Sugiyama layout of `nodes` (sizes[i] is the box of nodes[i]); `entry` seeds the back-edge DFS.
LayeredLayout layeredLayout(
	const std::vector<uint64_t>& nodes,
	const std::vector<GPoint>& sizes,
	const std::vector<EdgeRef>& edges,
	std::optional<uint64_t> entry,
	const LayeredOptions& options = {});

// ── Components, depths, neighbourhoods ───────────────────────────────────────

/// Tarjan's strongly connected components; each component lists its members, components in completion order.
std::vector<std::vector<uint64_t>>
stronglyConnectedComponents(const std::vector<uint64_t>& nodes, const Adjacency& succ);

/// BFS depth from the roots (nodes without predecessors; the first node when every node has one).
/// Unreachable nodes are absent.
std::unordered_map<uint64_t, int> depthFromRoots(const std::vector<uint64_t>& nodes, const GraphAdjacency& adj);

enum class NeighbourDirection : uint8_t
{
	Both,
	Successors,  ///< callees
	Predecessors ///< callers
};
/// Nodes within `hops` edges of `start` (start included).
std::unordered_set<uint64_t>
neighbourhood(uint64_t start, int hops, const GraphAdjacency& adj, NeighbourDirection dir = NeighbourDirection::Both);

// ── Force-directed layout ────────────────────────────────────────────────────

struct ForceNode
{
	uint64_t id = 0;
	float x = 0.f, y = 0.f;
	float fx = 0.f, fy = 0.f; ///< force accumulator (scratch)
};
/// Fruchterman-Reingold spring embedding as in the Qt panel (attraction d^2/k, repulsion k^2/d, temperature
/// sqrt(area)/4 cooled linearly), with ideal edge length `idealEdge` (area = k^2 n). Above `gridThreshold` nodes,
/// repulsion is limited to a 2k neighbourhood on a uniform grid (O(n) per iteration).
void forceDirectedLayout(
	std::vector<ForceNode>& nodes,
	const std::vector<EdgeRef>& edges,
	int iterations = 60,
	size_t gridThreshold = 400,
	double idealEdge = 170.0);

} // namespace retdec::gui::model

#endif // RETDEC_GUI_MODEL_GRAPH_LAYOUT_H
