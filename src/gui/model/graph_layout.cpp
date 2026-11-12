/**
 * @file src/gui/model/graph_layout.cpp
 * @brief Layered (Sugiyama) layout, SCCs, BFS depths / neighbourhoods and force-directed layout.
 * @copyright (c) 2026 Odin Loch trading as Imortek
 */

#include "retdec/gui/model/graph_layout.h"

#include <algorithm>
#include <cmath>
#include <deque>
#include <limits>
#include <map>
#include <numeric>
#include <random>

namespace retdec::gui::model {

namespace {
const std::vector<uint64_t>& emptyList()
{
	static const std::vector<uint64_t> e;
	return e;
}
} // namespace

GraphAdjacency GraphAdjacency::build(const std::vector<EdgeRef>& edges)
{
	GraphAdjacency a;
	for (const EdgeRef& e: edges)
	{
		a.succ[e.from].push_back(e.to);
		a.pred[e.to].push_back(e.from);
	}
	return a;
}

const std::vector<uint64_t>& GraphAdjacency::successors(uint64_t n) const
{
	const auto it = succ.find(n);
	return it == succ.end() ? emptyList() : it->second;
}

const std::vector<uint64_t>& GraphAdjacency::predecessors(uint64_t n) const
{
	const auto it = pred.find(n);
	return it == pred.end() ? emptyList() : it->second;
}

// ── Back edges ───────────────────────────────────────────────────────────────

EdgeSet findBackEdges(const std::vector<uint64_t>& nodes, const Adjacency& succ, std::optional<uint64_t> entry)
{
	EdgeSet back;
	// 0 = white, 1 = grey (on the DFS stack), 2 = black
	std::unordered_map<uint64_t, uint8_t> color;
	color.reserve(nodes.size());
	std::vector<std::pair<uint64_t, size_t>> stack;

	auto dfs = [&](uint64_t start) {
		if (color[start] != 0) return;
		color[start] = 1;
		stack.push_back({start, 0});
		while (!stack.empty())
		{
			auto& [u, idx] = stack.back();
			const auto sit = succ.find(u);
			bool pushed = false;
			if (sit != succ.end())
			{
				while (idx < sit->second.size())
				{
					const uint64_t v = sit->second[idx++];
					const uint8_t cv = color[v];
					if (cv == 1)
						back.insert({u, v});
					else if (cv == 0)
					{
						color[v] = 1;
						stack.push_back({v, 0});
						pushed = true;
						break;
					}
				}
			}
			if (!pushed)
			{
				color[stack.back().first] = 2;
				stack.pop_back();
			}
		}
	};

	if (entry) dfs(*entry);
	for (uint64_t n: nodes)
		dfs(n);
	return back;
}

std::unordered_map<uint64_t, int> longestPathRanks(
	const std::vector<uint64_t>& nodes, const Adjacency& succ, const EdgeSet& backEdges)
{
	std::unordered_map<uint64_t, int> indeg, rank;
	std::unordered_set<uint64_t> known(nodes.begin(), nodes.end());
	for (uint64_t n: nodes)
	{
		indeg[n];
		rank[n] = 0;
	}
	for (uint64_t u: nodes)
	{
		const auto it = succ.find(u);
		if (it == succ.end()) continue;
		for (uint64_t v: it->second)
			if (known.count(v) && !backEdges.count({u, v})) ++indeg[v];
	}
	// Kahn's algorithm in input order (deterministic).
	std::deque<uint64_t> q;
	for (uint64_t n: nodes)
		if (indeg[n] == 0) q.push_back(n);
	size_t processed = 0;
	while (!q.empty())
	{
		const uint64_t u = q.front();
		q.pop_front();
		++processed;
		const auto it = succ.find(u);
		if (it == succ.end()) continue;
		for (uint64_t v: it->second)
		{
			if (!known.count(v) || backEdges.count({u, v})) continue;
			rank[v] = std::max(rank[v], rank[u] + 1);
			if (--indeg[v] == 0) q.push_back(v);
		}
	}
	(void)processed; // with DFS back edges removed the graph is acyclic, so every node is processed
	return rank;
}

void barycentricSweep(std::vector<std::vector<uint64_t>>& levels, const Adjacency& pred, const Adjacency& succ, int sweeps)
{
	auto pass = [](std::vector<uint64_t>& lvl, const std::vector<uint64_t>& ref, const Adjacency& nb) {
		std::unordered_map<uint64_t, int> pos;
		for (int i = 0; i < int(ref.size()); ++i)
			pos[ref[size_t(i)]] = i;
		std::unordered_map<uint64_t, double> key;
		for (int i = 0; i < int(lvl.size()); ++i)
		{
			const uint64_t v = lvl[size_t(i)];
			double sum = 0;
			int cnt = 0;
			if (const auto it = nb.find(v); it != nb.end())
				for (uint64_t n: it->second)
					if (const auto p = pos.find(n); p != pos.end())
					{
						sum += p->second;
						++cnt;
					}
			// No neighbour on the reference level: keep the node's relative place.
			key[v] = cnt ? sum / cnt : double(i) * double(ref.size()) / double(std::max<size_t>(1, lvl.size()));
		}
		std::stable_sort(lvl.begin(), lvl.end(), [&](uint64_t a, uint64_t b) { return key[a] < key[b]; });
	};
	for (int s = 0; s < sweeps; ++s)
	{
		for (size_t li = 1; li < levels.size(); ++li)
			pass(levels[li], levels[li - 1], pred);
		for (size_t li = levels.size() >= 2 ? levels.size() - 1 : 0; li-- > 0;)
			pass(levels[li], levels[li + 1], succ);
	}
}

int countCrossings(const std::vector<std::vector<uint64_t>>& levels, const Adjacency& succ)
{
	int crossings = 0;
	for (size_t li = 0; li + 1 < levels.size(); ++li)
	{
		std::unordered_map<uint64_t, int> lower;
		for (int i = 0; i < int(levels[li + 1].size()); ++i)
			lower[levels[li + 1][size_t(i)]] = i;
		std::vector<std::pair<int, int>> seg;
		for (int i = 0; i < int(levels[li].size()); ++i)
			if (const auto it = succ.find(levels[li][size_t(i)]); it != succ.end())
				for (uint64_t v: it->second)
					if (const auto p = lower.find(v); p != lower.end()) seg.push_back({i, p->second});
		for (size_t a = 0; a < seg.size(); ++a)
			for (size_t b = a + 1; b < seg.size(); ++b)
				if ((seg[a].first < seg[b].first && seg[a].second > seg[b].second)
					|| (seg[a].first > seg[b].first && seg[a].second < seg[b].second))
					++crossings;
	}
	return crossings;
}

LayeredLayout layeredLayout(const std::vector<uint64_t>& nodes, const std::vector<GPoint>& sizes,
	const std::vector<EdgeRef>& edges, std::optional<uint64_t> entry, const LayeredOptions& opt)
{
	LayeredLayout out;
	if (nodes.empty()) return out;

	std::unordered_map<uint64_t, GPoint> size;
	for (size_t i = 0; i < nodes.size(); ++i)
		size[nodes[i]] = i < sizes.size() ? sizes[i] : GPoint{100.f, 40.f};

	// Only edges between known nodes take part.
	std::vector<EdgeRef> inner;
	inner.reserve(edges.size());
	for (const EdgeRef& e: edges)
		if (size.count(e.from) && size.count(e.to) && e.from != e.to) inner.push_back(e);
	const GraphAdjacency adj = GraphAdjacency::build(inner);

	out.backEdges = findBackEdges(nodes, adj.succ, entry);
	out.rank = longestPathRanks(nodes, adj.succ, out.backEdges);

	int maxRank = 0;
	for (const auto& [id, r]: out.rank)
		maxRank = std::max(maxRank, r);
	out.levels.assign(size_t(maxRank + 1), {});
	for (uint64_t n: nodes)
		out.levels[size_t(out.rank[n])].push_back(n);

	// Crossing minimisation on the forward graph (back edges reversed would pull loops up; ignoring them keeps the
	// Qt behaviour).
	Adjacency fwdSucc, fwdPred;
	for (const EdgeRef& e: inner)
	{
		if (out.backEdges.count({e.from, e.to})) continue;
		fwdSucc[e.from].push_back(e.to);
		fwdPred[e.to].push_back(e.from);
	}
	barycentricSweep(out.levels, fwdPred, fwdSucc, opt.sweeps);

	// ── x: packed, centred levels, then neighbour alignment ──────────────────
	std::unordered_map<uint64_t, float> cx; // centre x
	for (auto& lvl: out.levels)
	{
		float total = 0.f;
		for (uint64_t n: lvl)
			total += size[n].x;
		total += opt.nodeGap * float(lvl.empty() ? 0 : lvl.size() - 1);
		float x = -total * 0.5f;
		for (uint64_t n: lvl)
		{
			cx[n] = x + size[n].x * 0.5f;
			x += size[n].x + opt.nodeGap;
		}
	}
	auto place = [&](std::vector<uint64_t>& lvl, const Adjacency& nb) {
		if (lvl.empty()) return;
		std::vector<float> desired(lvl.size()), pos(lvl.size());
		for (size_t i = 0; i < lvl.size(); ++i)
		{
			const uint64_t v = lvl[i];
			float sum = 0.f;
			int cnt = 0;
			if (const auto it = nb.find(v); it != nb.end())
				for (uint64_t n: it->second)
				{
					sum += cx[n];
					++cnt;
				}
			desired[i] = cnt ? sum / float(cnt) : cx[v];
		}
		// Keep order and separation, then shift the level so the mean displacement is zero.
		for (size_t i = 0; i < lvl.size(); ++i)
		{
			pos[i] = desired[i];
			if (i > 0)
			{
				const float minC = pos[i - 1] + size[lvl[i - 1]].x * 0.5f + opt.nodeGap + size[lvl[i]].x * 0.5f;
				pos[i] = std::max(pos[i], minC);
			}
		}
		float shift = 0.f;
		for (size_t i = 0; i < lvl.size(); ++i)
			shift += pos[i] - desired[i];
		shift /= float(lvl.size());
		for (size_t i = 0; i < lvl.size(); ++i)
			cx[lvl[i]] = pos[i] - shift;
	};
	for (int p = 0; p < opt.alignPasses; ++p)
	{
		for (size_t li = 1; li < out.levels.size(); ++li)
			place(out.levels[li], fwdPred);
		for (size_t li = out.levels.size() >= 2 ? out.levels.size() - 1 : 0; li-- > 0;)
			place(out.levels[li], fwdSucc);
	}

	// ── y: stacked levels ────────────────────────────────────────────────────
	float minX = std::numeric_limits<float>::max();
	for (uint64_t n: nodes)
		minX = std::min(minX, cx[n] - size[n].x * 0.5f);
	float y = opt.margin;
	for (const auto& lvl: out.levels)
	{
		float h = 0.f;
		for (uint64_t n: lvl)
		{
			out.topLeft[n] = {cx[n] - size[n].x * 0.5f - minX + opt.margin, y};
			h = std::max(h, size[n].y);
			out.bounds = out.bounds.united({out.topLeft[n].x, y, size[n].x, size[n].y});
		}
		y += h + opt.rankGap;
	}
	return out;
}

// ── SCC ──────────────────────────────────────────────────────────────────────

std::vector<std::vector<uint64_t>> stronglyConnectedComponents(const std::vector<uint64_t>& nodes, const Adjacency& succ)
{
	std::vector<std::vector<uint64_t>> result;
	std::unordered_map<uint64_t, int> index, lowlink;
	std::unordered_set<uint64_t> onStack;
	std::vector<uint64_t> stk;
	int counter = 0;
	struct Frame
	{
		uint64_t node;
		size_t succIdx;
	};
	for (uint64_t start: nodes)
	{
		if (index.count(start)) continue;
		std::vector<Frame> call;
		call.push_back({start, 0});
		index[start] = lowlink[start] = counter++;
		stk.push_back(start);
		onStack.insert(start);
		while (!call.empty())
		{
			const uint64_t u = call.back().node;
			const auto sit = succ.find(u);
			bool pushed = false;
			if (sit != succ.end())
			{
				while (call.back().succIdx < sit->second.size())
				{
					const uint64_t v = sit->second[call.back().succIdx++];
					if (!index.count(v))
					{
						index[v] = lowlink[v] = counter++;
						stk.push_back(v);
						onStack.insert(v);
						call.push_back({v, 0});
						pushed = true;
						break;
					}
					if (onStack.count(v)) lowlink[u] = std::min(lowlink[u], index[v]);
				}
			}
			if (pushed) continue;
			if (lowlink[u] == index[u])
			{
				std::vector<uint64_t> scc;
				uint64_t w = 0;
				do
				{
					w = stk.back();
					stk.pop_back();
					onStack.erase(w);
					scc.push_back(w);
				}
				while (w != u);
				result.push_back(std::move(scc));
			}
			call.pop_back();
			if (!call.empty()) lowlink[call.back().node] = std::min(lowlink[call.back().node], lowlink[u]);
		}
	}
	return result;
}

// ── Depths and neighbourhoods ────────────────────────────────────────────────

std::unordered_map<uint64_t, int> depthFromRoots(const std::vector<uint64_t>& nodes, const GraphAdjacency& adj)
{
	std::unordered_map<uint64_t, int> depth;
	std::deque<uint64_t> q;
	for (uint64_t n: nodes)
		if (adj.predecessors(n).empty())
		{
			depth[n] = 0;
			q.push_back(n);
		}
	if (q.empty() && !nodes.empty())
	{
		depth[nodes.front()] = 0;
		q.push_back(nodes.front());
	}
	while (!q.empty())
	{
		const uint64_t u = q.front();
		q.pop_front();
		const int d = depth[u];
		for (uint64_t v: adj.successors(u))
		{
			const auto it = depth.find(v);
			if (it == depth.end() || d + 1 < it->second)
			{
				depth[v] = d + 1;
				q.push_back(v);
			}
		}
	}
	return depth;
}

std::unordered_set<uint64_t> neighbourhood(uint64_t start, int hops, const GraphAdjacency& adj, NeighbourDirection dir)
{
	std::unordered_set<uint64_t> seen{start};
	std::deque<std::pair<uint64_t, int>> q{{start, 0}};
	while (!q.empty())
	{
		const auto [n, d] = q.front();
		q.pop_front();
		if (d >= hops) continue;
		auto visit = [&](const std::vector<uint64_t>& list) {
			for (uint64_t m: list)
				if (seen.insert(m).second) q.push_back({m, d + 1});
		};
		if (dir != NeighbourDirection::Predecessors) visit(adj.successors(n));
		if (dir != NeighbourDirection::Successors) visit(adj.predecessors(n));
	}
	return seen;
}

// ── Force-directed ───────────────────────────────────────────────────────────

void forceDirectedLayout(std::vector<ForceNode>& nodes, const std::vector<EdgeRef>& edges, int iterations, size_t gridThreshold,
	double idealEdge)
{
	if (nodes.empty() || iterations <= 0) return;
	const double n = double(nodes.size());
	// Ideal edge length k. (The Qt panel used area = 900^2 * sqrt(n), i.e. k = 900 / n^0.25: 535 px for 8 nodes, so a
	// fitted view of a small graph showed unreadably small boxes. A fixed k keeps boxes readable at fit.)
	const double k = idealEdge;
	const double area = k * k * n;
	double temperature = std::sqrt(area) / 4.0;
	const double cooling = temperature / double(iterations);
	const bool useGrid = nodes.size() > gridThreshold;
	const double cell = 2.0 * k;

	std::unordered_map<uint64_t, size_t> idx;
	for (size_t i = 0; i < nodes.size(); ++i)
		idx[nodes[i].id] = i;
	std::vector<std::pair<size_t, size_t>> springs;
	springs.reserve(edges.size());
	for (const EdgeRef& e: edges)
	{
		const auto a = idx.find(e.from), b = idx.find(e.to);
		if (a != idx.end() && b != idx.end() && a->second != b->second) springs.push_back({a->second, b->second});
	}

	auto repulse = [&](ForceNode& a, ForceNode& b) {
		double dx = a.x - b.x, dy = a.y - b.y;
		double d = std::sqrt(dx * dx + dy * dy);
		if (d < 0.01)
		{
			d = 0.01;
			dx = 0.01;
			dy = 0.01;
		}
		if (useGrid && d > cell) return;
		const double f = (k * k / d) / d;
		a.fx += float(f * dx);
		a.fy += float(f * dy);
		b.fx -= float(f * dx);
		b.fy -= float(f * dy);
	};

	std::map<std::pair<long, long>, std::vector<size_t>> grid;
	for (int iter = 0; iter < iterations; ++iter)
	{
		for (ForceNode& nd: nodes)
			nd.fx = nd.fy = 0.f;

		if (!useGrid)
		{
			for (size_t i = 0; i < nodes.size(); ++i)
				for (size_t j = i + 1; j < nodes.size(); ++j)
					repulse(nodes[i], nodes[j]);
		}
		else
		{
			grid.clear();
			for (size_t i = 0; i < nodes.size(); ++i)
				grid[{long(std::floor(nodes[i].x / cell)), long(std::floor(nodes[i].y / cell))}].push_back(i);
			for (const auto& [key, members]: grid)
			{
				for (long gx = key.first - 1; gx <= key.first + 1; ++gx)
					for (long gy = key.second - 1; gy <= key.second + 1; ++gy)
					{
						const auto it = grid.find({gx, gy});
						if (it == grid.end()) continue;
						for (size_t i: members)
							for (size_t j: it->second)
								if (i < j) repulse(nodes[i], nodes[j]);
					}
			}
		}

		for (const auto& [a, b]: springs)
		{
			const double dx = nodes[a].x - nodes[b].x, dy = nodes[a].y - nodes[b].y;
			const double d = std::sqrt(dx * dx + dy * dy);
			if (d < 0.01) continue;
			const double f = (d * d / k) / d;
			nodes[a].fx -= float(f * dx);
			nodes[a].fy -= float(f * dy);
			nodes[b].fx += float(f * dx);
			nodes[b].fy += float(f * dy);
		}

		for (ForceNode& nd: nodes)
		{
			const double fm = std::sqrt(double(nd.fx) * nd.fx + double(nd.fy) * nd.fy);
			if (fm > 0)
			{
				const double s = std::min(fm, temperature) / fm;
				nd.x += float(nd.fx * s);
				nd.y += float(nd.fy * s);
			}
		}
		temperature = std::max(0.0, temperature - cooling);
	}
}

} // namespace retdec::gui::model
