/**
 * @file src/gui/model/callgraph_model.cpp
 * @brief Call-graph SCC collapsing, layouts, scene building, filters, focus and DOT export.
 * @copyright (c) 2026 Odin Loch trading as Imortek
 */

#include "retdec/gui/model/callgraph_model.h"

#include "retdec/gui/model/graph_search.h"

#include <algorithm>
#include <cmath>
#include <format>
#include <limits>
#include <map>
#include <random>

namespace retdec::gui::model {

Rgba callGraphModuleColor(int moduleId)
{
	static constexpr Rgba kPalette[] = {
		catppuccin::blue,
		catppuccin::green,
		catppuccin::mauve,
		catppuccin::yellow,
		catppuccin::peach,
		catppuccin::teal,
		catppuccin::pink,
		catppuccin::flamingo,
		catppuccin::rosewater,
		catppuccin::overlay0,
	};
	if (moduleId < 0) return catppuccin::surface2;
	return kPalette[size_t(moduleId) % std::size(kPalette)];
}

float callGraphNodeWidth(int instrCount)
{
	if (instrCount <= 0) return callgraph::kMinNodeW;
	const float scaled = callgraph::kMinNodeW + float(std::log2(1.0 + instrCount)) * 10.f;
	return std::min(scaled, callgraph::kMaxNodeW);
}

float callGraphEdgeWidth(int callCount)
{
	return std::min(1.f + float(std::log2(1.0 + std::max(0, callCount))) * 0.4f, 4.f);
}

namespace {

std::string htmlEscape(std::string_view s)
{
	std::string out;
	for (char c: s)
	{
		switch (c)
		{
		case '&': out += "&amp;"; break;
		case '<': out += "&lt;"; break;
		case '>': out += "&gt;"; break;
		case '"': out += "&quot;"; break;
		default: out += c;
		}
	}
	return out;
}

/// Cubic route between two boxes: vertical ports (bottom -> top) when the target is clearly below, top -> bottom when
/// clearly above (layered: an arc on the left instead), side ports when the boxes are mostly side by side.
std::vector<GPoint> routeBetween(const GRect& fr, const GRect& tr, bool layered)
{
	const GPoint fc = fr.center(), tc = tr.center();
	const float dx = tc.x - fc.x, dy = tc.y - fc.y;
	const bool below = tr.y >= fr.bottom() + 4.f, above = tr.bottom() <= fr.y - 4.f;
	if (layered && !below)
	{
		const GPoint from{fr.x, fc.y}, to{tr.x, tc.y};
		const float leftX = std::min(fr.x, tr.x) - 50.f;
		return {from, {leftX, from.y}, {leftX, to.y}, to};
	}
	if (below || (above && std::abs(dy) >= std::abs(dx) * 0.35f))
	{
		const GPoint from{fc.x, below ? fr.bottom() : fr.y}, to{tc.x, below ? tr.y : tr.bottom()};
		const float cy = (from.y + to.y) * 0.5f;
		return {from, {from.x, cy}, {to.x, cy}, to};
	}
	const bool right = dx >= 0.f;
	const GPoint from{right ? fr.right() : fr.x, fc.y}, to{right ? tr.x : tr.right(), tc.y};
	const float cx = (from.x + to.x) * 0.5f;
	return {from, {cx, from.y}, {cx, to.y}, to};
}

} // namespace

void CallGraphModel::clearGraph()
{
	m_nodes.clear();
	m_edges.clear();
	m_byAddr.clear();
	m_adj = {};
	m_depth.clear();
	m_focused = false;
	m_focusAddress = 0;
	m_focusSet.clear();
	m_scene.clear();
	m_sccMembers.clear();
	m_repOf.clear();
	m_edgeCalls.clear();
	m_drawn.clear();
}

void CallGraphModel::loadGraph(std::vector<CallGraphNode> nodes, std::vector<CallEdge> edges)
{
	clearGraph();
	m_nodes = std::move(nodes);
	m_edges = std::move(edges);
	for (size_t i = 0; i < m_nodes.size(); ++i)
		m_byAddr.emplace(m_nodes[i].address, i);
	std::vector<EdgeRef> refs;
	refs.reserve(m_edges.size());
	for (const CallEdge& e: m_edges)
		refs.push_back({e.callerAddress, e.calleeAddress});
	m_adj = GraphAdjacency::build(refs);
	recomputeDepths();
	relayout();
}

void CallGraphModel::recomputeDepths()
{
	std::vector<uint64_t> addrs;
	addrs.reserve(m_nodes.size());
	for (const auto& n: m_nodes)
		addrs.push_back(n.address);
	m_depth = depthFromRoots(addrs, m_adj);
}

void CallGraphModel::relayout()
{
	m_scene.clear();
	m_sccMembers.clear();
	m_repOf.clear();
	m_edgeCalls.clear();
	m_drawn.clear();
	if (m_nodes.empty()) return;

	// Display limit.
	const size_t visCount = std::min(m_nodes.size(), size_t(callgraph::kMaxNodes));
	std::unordered_set<uint64_t> nodeSet;
	std::vector<uint64_t> addrs;
	addrs.reserve(visCount);
	for (size_t i = 0; i < visCount; ++i)
		if (nodeSet.insert(m_nodes[i].address).second) addrs.push_back(m_nodes[i].address);
	Adjacency succ;
	for (const CallEdge& e: m_edges)
		if (nodeSet.count(e.callerAddress) && nodeSet.count(e.calleeAddress))
			succ[e.callerAddress].push_back(e.calleeAddress);

	// ── SCC collapse (representative: first member in input order) ───────────
	if (m_collapseSccs)
	{
		std::unordered_map<uint64_t, size_t> pos;
		for (size_t i = 0; i < addrs.size(); ++i)
			pos[addrs[i]] = i;
		for (auto& scc: stronglyConnectedComponents(addrs, succ))
		{
			if (scc.size() < 2) continue;
			std::sort(scc.begin(), scc.end(), [&](uint64_t a, uint64_t b) { return pos[a] < pos[b]; });
			const uint64_t rep = scc.front();
			for (uint64_t a: scc)
				m_repOf[a] = rep;
			m_sccMembers[rep] = std::move(scc);
		}
	}
	auto resolve = [&](uint64_t a) {
		const auto it = m_repOf.find(a);
		return it == m_repOf.end() ? a : it->second;
	};

	// ── Layout nodes ─────────────────────────────────────────────────────────
	std::vector<uint64_t> layoutIds;
	std::vector<GPoint> sizes;
	for (uint64_t a: addrs)
	{
		if (m_repOf.count(a) && m_repOf[a] != a) continue;
		layoutIds.push_back(a);
		if (m_sccMembers.count(a))
			sizes.push_back({callgraph::kSccW, callgraph::kSccH});
		else
		{
			sizes.push_back({callGraphNodeWidth(m_nodes[m_byAddr[a]].instrCount), callgraph::kNodeH});
			m_drawn.insert(a);
		}
	}
	std::vector<EdgeRef> layoutEdges;
	for (const CallEdge& e: m_edges)
	{
		if (!nodeSet.count(e.callerAddress) || !nodeSet.count(e.calleeAddress)) continue;
		const uint64_t u = resolve(e.callerAddress), v = resolve(e.calleeAddress);
		if (u != v) layoutEdges.push_back({u, v});
	}

	std::unordered_map<uint64_t, GPoint> topLeft;
	if (m_mode == CallGraphLayoutMode::Layered)
	{
		LayeredOptions lo;
		lo.nodeGap = 30.f;
		lo.rankGap = 70.f;
		lo.margin = callgraph::kMargin;
		const LayeredLayout lay = layeredLayout(layoutIds, sizes, layoutEdges, std::nullopt, lo);
		topLeft = lay.topLeft;
	}
	else
	{
		std::mt19937 rng(42);
		std::uniform_real_distribution<float> rnd(-200.f, 200.f);
		std::vector<ForceNode> fn;
		fn.reserve(layoutIds.size());
		// Plain nodes first, then SCC super-nodes (the Qt order, so seeds match).
		for (uint64_t id: layoutIds)
			if (!m_sccMembers.count(id))
			{
				ForceNode f;
				f.id = id;
				f.x = rnd(rng);
				f.y = rnd(rng);
				fn.push_back(f);
			}
		for (uint64_t id: layoutIds)
			if (m_sccMembers.count(id))
			{
				ForceNode f;
				f.id = id;
				f.x = rnd(rng);
				f.y = rnd(rng);
				fn.push_back(f);
			}
		forceDirectedLayout(fn, layoutEdges, fn.size() > 300 ? 20 : 60);
		float minX = std::numeric_limits<float>::max(), minY = minX;
		for (const auto& f: fn)
		{
			minX = std::min(minX, f.x);
			minY = std::min(minY, f.y);
		}
		for (const auto& f: fn)
			topLeft[f.id] = {f.x - minX + callgraph::kMargin, f.y - minY + callgraph::kMargin};
	}

	// ── Scene nodes ──────────────────────────────────────────────────────────
	for (size_t i = 0; i < layoutIds.size(); ++i)
	{
		const uint64_t id = layoutIds[i];
		const GPoint p = topLeft.count(id) ? topLeft[id] : GPoint{};
		SceneNode sn;
		sn.id = id;
		sn.rect = {p.x, p.y, sizes[i].x, sizes[i].y};
		if (const auto sit = m_sccMembers.find(id); sit != m_sccMembers.end())
		{
			const auto& members = sit->second;
			sn.kind = callgraph::Scc;
			sn.radius = 6.f;
			sn.fill = rgbaWithAlpha(catppuccin::peach, 40);
			sn.hoverFill = rgbaWithAlpha(catppuccin::peach, 70);
			sn.border = catppuccin::peach;
			sn.borderWidth = 1.8f;
			sn.dashedBorder = true;
			sn.shadow = false;
			const std::string& first = m_nodes[m_byAddr[members.front()]].name;
			sn.texts.push_back(
				SceneText{
					std::format("SCC [{}]", members.size()),
					{sn.rect.x, sn.rect.y + 3.f, sn.rect.w, 18.f},
					11.f,
					catppuccin::peach,
					SceneAlign::Center,
					true,
					true});
			sn.texts.push_back(
				SceneText{
					first,
					{sn.rect.x + 4.f, sn.rect.y + 20.f, sn.rect.w - 8.f, 16.f},
					9.5f,
					catppuccin::subtext0,
					SceneAlign::Center,
					false,
					true});
			std::string tip = std::format("SCC: {} mutually recursive functions", members.size());
			for (size_t k = 0; k < std::min<size_t>(members.size(), 10); ++k)
				tip += "\n  " + m_nodes[m_byAddr[members[k]]].name;
			if (members.size() > 10) tip += std::format("\n  … +{} more", members.size() - 10);
			tip += "\nDouble-click to expand";
			sn.tooltip = std::move(tip);
			for (uint64_t m: members)
			{
				sn.searchText += m_nodes[m_byAddr[m]].name;
				sn.searchText += '\n';
			}
		}
		else
		{
			const CallGraphNode& n = m_nodes[m_byAddr[id]];
			const Rgba mod = callGraphModuleColor(n.moduleId);
			sn.kind = callgraph::Function;
			sn.radius = 5.f;
			sn.fill = n.isLibrary ? catppuccin::surface0 : catppuccin::surface1;
			sn.border = n.isLibrary ? catppuccin::surface2 : mod;
			sn.hoverBorder = catppuccin::blue;
			sn.borderWidth = 1.5f;
			sn.shadow = false;
			if (!n.isLibrary)
			{
				sn.stripeFill = rgbaFade(mod, 0.8f);
				sn.stripeWidth = 4.f;
			}
			std::string count = n.instrCount > 0 ? std::to_string(n.instrCount) : std::string();
			const float countW = count.empty() ? 0.f : 6.f * float(count.size()) + 6.f;
			sn.texts.push_back(
				SceneText{
					n.name,
					{sn.rect.x + 8.f, sn.rect.y, sn.rect.w - 14.f - countW, sn.rect.h},
					11.f,
					n.isLibrary ? catppuccin::subtext0 : catppuccin::text,
					SceneAlign::Left,
					false,
					true});
			if (!count.empty())
				sn.texts.push_back(
					SceneText{
						count,
						{sn.rect.x + 4.f, sn.rect.y, sn.rect.w - 10.f, sn.rect.h},
						9.5f,
						catppuccin::overlay0,
						SceneAlign::Right,
						false,
						true});
			sn.tooltip = std::format(
				"{}\n{}\n{} instrs{}\n{} caller{}, {} callee{}",
				n.name,
				hexAddress(n.address),
				n.instrCount,
				n.isLibrary ? "\n[library]" : "",
				callerCount(n.address),
				callerCount(n.address) == 1 ? "" : "s",
				calleeCount(n.address),
				calleeCount(n.address) == 1 ? "" : "s");
			if (n.moduleId >= 0) sn.tooltip += std::format("\nModule {}", n.moduleId);
			sn.searchText = n.name;
		}
		m_scene.nodes.push_back(std::move(sn));
	}
	std::unordered_map<uint64_t, GRect> rectOf;
	for (const SceneNode& n: m_scene.nodes)
		rectOf[n.id] = n.rect;

	// ── Scene edges (deduplicated, call counts aggregated) ───────────────────
	std::map<std::pair<uint64_t, uint64_t>, int> calls;
	for (const CallEdge& e: m_edges)
	{
		if (!nodeSet.count(e.callerAddress) || !nodeSet.count(e.calleeAddress)) continue;
		const uint64_t u = resolve(e.callerAddress), v = resolve(e.calleeAddress);
		if (u != v) calls[{u, v}] += e.callCount;
	}
	EdgeSet drawnEdges;
	for (const CallEdge& e: m_edges)
	{
		if (!nodeSet.count(e.callerAddress) || !nodeSet.count(e.calleeAddress)) continue;
		const uint64_t u = resolve(e.callerAddress), v = resolve(e.calleeAddress);
		if (u == v || !drawnEdges.insert({u, v}).second) continue;
		const GRect fr = rectOf[u], tr = rectOf[v];
		SceneEdge se;
		se.from = u;
		se.to = v;
		const int count = calls[{u, v}];
		se.width = callGraphEdgeWidth(count);
		se.color = e.isDirect ? rgbaWithAlpha(catppuccin::blue, 160) : rgbaWithAlpha(catppuccin::subtext0, 120);
		se.dashed = !e.isDirect;
		se.dash = 5.f;
		se.gap = 3.f;
		se.kind = e.isDirect ? 0 : 1;
		se.arrowLength = 9.f;
		se.arrowHalfWidth = 4.5f;
		se.points = routeBetween(fr, tr, m_mode == CallGraphLayoutMode::Layered);
		m_scene.edges.push_back(std::move(se));
		m_edgeCalls.push_back(count);
	}

	// ── Module cluster backdrops (rects recomputed from visible members in applyFilters) ──
	std::map<int, bool> modules;
	for (uint64_t a: m_drawn)
		if (const int mid = m_nodes[m_byAddr[a]].moduleId; mid >= 0) modules[mid] = true;
	for (const auto& [mid, unused]: modules)
	{
		const Rgba c = callGraphModuleColor(mid);
		SceneRegion r;
		r.fill = rgbaWithAlpha(c, 18);
		r.border = rgbaWithAlpha(c, 100);
		r.borderWidth = 1.5f;
		r.label = std::format("Module {}", mid);
		r.labelColor = rgbaWithAlpha(c, 180);
		r.group = mid;
		m_scene.regions.push_back(r);
	}

	applyFilters();
	// Region rects were set by applyFilters from all drawn members; finalize with every region at full extent.
	for (SceneRegion& r: m_scene.regions)
	{
		GRect all;
		for (uint64_t a: m_drawn)
			if (m_nodes[m_byAddr[a]].moduleId == r.group) all = all.united(rectOf[a]);
		r.rect = all.adjusted(-15.f, -20.f, 15.f, 15.f);
	}
	m_scene.finalize(callgraph::kMargin);
	applyFilters();
}

bool CallGraphModel::isNodeVisible(uint64_t addr) const
{
	if (m_focused && !m_focusSet.count(addr)) return false;
	const auto it = m_byAddr.find(addr);
	if (it == m_byAddr.end()) return true;
	const CallGraphNode& n = m_nodes[it->second];
	if (m_hideLibrary && n.isLibrary) return false;
	if (m_moduleFilter >= 0 && n.moduleId != m_moduleFilter) return false;
	if (!m_nameFilter.empty() && !graphNameFilterMatches(m_nameFilter, n.name)) return false;
	if (m_maxDepth >= 0)
		if (const auto d = m_depth.find(addr); d != m_depth.end() && d->second > m_maxDepth) return false;
	return true;
}

void CallGraphModel::applyFilters()
{
	std::unordered_map<uint64_t, bool> vis;
	for (SceneNode& sn: m_scene.nodes)
	{
		bool v = false;
		if (const auto it = m_sccMembers.find(sn.id); it != m_sccMembers.end())
		{
			for (uint64_t m: it->second)
				v = v || isNodeVisible(m);
		}
		else
			v = isNodeVisible(sn.id);
		sn.visible = v;
		sn.opacity = 1.f;
		vis[sn.id] = v;
	}
	for (size_t i = 0; i < m_scene.edges.size(); ++i)
	{
		SceneEdge& e = m_scene.edges[i];
		e.visible = vis[e.from] && vis[e.to] && m_edgeCalls[i] >= m_minCallCount;
	}
	for (SceneRegion& r: m_scene.regions)
	{
		GRect box;
		for (const SceneNode& sn: m_scene.nodes)
			if (sn.visible && sn.kind == callgraph::Function && m_nodes[m_byAddr.at(sn.id)].moduleId == r.group)
				box = box.united(sn.rect);
		r.visible = !box.empty();
		if (r.visible) r.rect = box.adjusted(-15.f, -20.f, 15.f, 15.f);
	}
}

void CallGraphModel::setNameFilter(std::string glob)
{
	m_nameFilter = std::move(glob);
	applyFilters();
}

void CallGraphModel::setMinCallCount(int minCount)
{
	m_minCallCount = minCount;
	applyFilters();
}

void CallGraphModel::setHideLibrary(bool hide)
{
	m_hideLibrary = hide;
	applyFilters();
}

void CallGraphModel::setModuleFilter(int moduleId)
{
	m_moduleFilter = moduleId;
	applyFilters();
}

void CallGraphModel::setMaxDepth(int depth)
{
	m_maxDepth = depth;
	applyFilters();
}

void CallGraphModel::setFocusNode(uint64_t address, int hops)
{
	m_focused = true;
	m_focusAddress = address;
	m_focusHops = hops;
	m_focusSet = neighbourhood(address, hops, m_adj);
	applyFilters();
}

void CallGraphModel::clearFocus()
{
	m_focused = false;
	m_focusAddress = 0;
	m_focusSet.clear();
	applyFilters();
}

void CallGraphModel::setLayoutMode(CallGraphLayoutMode mode)
{
	if (mode == m_mode) return;
	m_mode = mode;
	relayout();
}

void CallGraphModel::setCollapseSccs(bool collapse)
{
	if (collapse == m_collapseSccs) return;
	m_collapseSccs = collapse;
	relayout();
}

int CallGraphModel::visibleNodeCount() const
{
	int count = 0;
	for (const SceneNode& sn: m_scene.nodes)
		if (sn.kind == callgraph::Function && sn.visible) ++count;
	return count;
}

int CallGraphModel::visibleSccCount() const
{
	int count = 0;
	for (const SceneNode& sn: m_scene.nodes)
		if (sn.kind == callgraph::Scc && sn.visible) ++count;
	return count;
}

const CallGraphNode* CallGraphModel::node(uint64_t address) const
{
	const auto it = m_byAddr.find(address);
	return it == m_byAddr.end() ? nullptr : &m_nodes[it->second];
}

std::optional<int> CallGraphModel::depthOf(uint64_t address) const
{
	const auto it = m_depth.find(address);
	if (it == m_depth.end()) return std::nullopt;
	return it->second;
}

std::vector<int> CallGraphModel::moduleIds() const
{
	std::vector<int> ids;
	for (const auto& n: m_nodes)
		if (n.moduleId >= 0) ids.push_back(n.moduleId);
	std::sort(ids.begin(), ids.end());
	ids.erase(std::unique(ids.begin(), ids.end()), ids.end());
	return ids;
}

std::optional<uint64_t> CallGraphModel::sceneNodeFor(uint64_t address) const
{
	if (const auto it = m_repOf.find(address); it != m_repOf.end()) return it->second;
	if (m_drawn.count(address)) return address;
	return std::nullopt;
}

int CallGraphModel::callerCount(uint64_t address) const
{
	return int(m_adj.predecessors(address).size());
}

int CallGraphModel::calleeCount(uint64_t address) const
{
	return int(m_adj.successors(address).size());
}

std::string CallGraphModel::exportDot() const
{
	std::string dot =
		"digraph callgraph {\n"
		"  graph [bgcolor=\"#1e1e2e\" fontcolor=\"#cdd6f4\"];\n"
		"  node [shape=box style=filled fillcolor=\"#313244\""
		" color=\"#45475a\" fontcolor=\"#cdd6f4\" fontname=\"Consolas\"];\n"
		"  edge [color=\"#89b4fa\" fontcolor=\"#cdd6f4\"];\n\n";
	for (const auto& n: m_nodes)
		dot += std::format(
			"  \"{}\" [label=\"{}\\n0x{:x}\"{}];\n",
			n.address,
			htmlEscape(n.name),
			n.address,
			n.isLibrary ? " style=dashed" : "");
	dot += "\n";
	EdgeSet seen;
	for (const auto& e: m_edges)
	{
		if (!seen.insert({e.callerAddress, e.calleeAddress}).second) continue;
		dot += std::format(
			"  \"{}\" -> \"{}\" [label=\"{}\"{}];\n",
			e.callerAddress,
			e.calleeAddress,
			e.callCount,
			e.isDirect ? "" : " style=dashed");
	}
	dot += "}\n";
	return dot;
}

} // namespace retdec::gui::model
