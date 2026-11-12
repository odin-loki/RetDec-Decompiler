/**
 * @file tests/gui/graph_layout_test.cpp
 * @brief Graph model tests: layered layout, back edges, ranks, crossings, SCCs, depths, neighbourhoods,
 *        force layout, scene geometry, view transform, search / globs, SVG export, natural loops, chain compression.
 * @copyright (c) 2026 Odin Loch trading as Imortek
 */

#include "retdec/gui/model/cfg_graph.h"
#include "retdec/gui/model/graph_export.h"
#include "retdec/gui/model/graph_layout.h"
#include "retdec/gui/model/graph_scene.h"
#include "retdec/gui/model/graph_search.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <format>
#include <unordered_map>

using namespace retdec::gui::model;

namespace {

std::vector<EdgeRef> E(std::initializer_list<std::pair<uint64_t, uint64_t>> l)
{
	std::vector<EdgeRef> out;
	for (auto [a, b]: l)
		out.push_back({a, b});
	return out;
}

std::vector<uint64_t> sorted(std::vector<uint64_t> v)
{
	std::sort(v.begin(), v.end());
	return v;
}

} // namespace

// ── Back edges and ranks ─────────────────────────────────────────────────────

TEST(GraphLayoutTest, BackEdgesFromEntryDfs)
{
	const auto adj = GraphAdjacency::build(E({{1, 2}, {2, 3}, {3, 2}, {3, 4}, {4, 1}}));
	const EdgeSet back = findBackEdges({1, 2, 3, 4}, adj.succ, 1);
	EXPECT_EQ(back.size(), 2u);
	EXPECT_TRUE(back.count({3, 2}));
	EXPECT_TRUE(back.count({4, 1}));
}

TEST(GraphLayoutTest, BackEdgesInUnreachableCycleAreFound)
{
	// 1 alone; 5 <-> 6 not reachable from the entry: the cycle must still be broken.
	const auto adj = GraphAdjacency::build(E({{5, 6}, {6, 5}}));
	const EdgeSet back = findBackEdges({1, 5, 6}, adj.succ, 1);
	EXPECT_EQ(back.size(), 1u);
	const auto ranks = longestPathRanks({1, 5, 6}, adj.succ, back);
	EXPECT_EQ(ranks.at(1), 0);
	EXPECT_NE(ranks.at(5), ranks.at(6));
}

TEST(GraphLayoutTest, LongestPathRanks)
{
	// 1 -> 2 -> 4, 1 -> 3 -> ... 1 -> 4 : 4 lands below both paths
	const auto adj = GraphAdjacency::build(E({{1, 2}, {2, 4}, {1, 4}, {1, 3}}));
	const auto r = longestPathRanks({1, 2, 3, 4}, adj.succ, {});
	EXPECT_EQ(r.at(1), 0);
	EXPECT_EQ(r.at(2), 1);
	EXPECT_EQ(r.at(3), 1);
	EXPECT_EQ(r.at(4), 2);
}

TEST(GraphLayoutTest, BarycentricSweepRemovesCrossings)
{
	// level 0: a b ; level 1: y x  with a->x, b->y  (one crossing until sorted)
	std::vector<std::vector<uint64_t>> levels{{1, 2}, {20, 10}};
	const auto adj = GraphAdjacency::build(E({{1, 10}, {2, 20}}));
	EXPECT_EQ(countCrossings(levels, adj.succ), 1);
	barycentricSweep(levels, adj.pred, adj.succ);
	EXPECT_EQ(countCrossings(levels, adj.succ), 0);
}

TEST(GraphLayoutTest, LayeredLayoutStacksLevelsWithoutOverlap)
{
	std::vector<uint64_t> nodes{1, 2, 3, 4, 5, 6};
	std::vector<GPoint> sizes(nodes.size(), GPoint{100.f, 40.f});
	const auto edges = E({{1, 2}, {1, 3}, {1, 4}, {2, 5}, {3, 5}, {4, 6}, {5, 6}, {6, 1}});
	LayeredOptions o;
	const auto lay = layeredLayout(nodes, sizes, edges, 1, o);
	EXPECT_TRUE(lay.backEdges.count({6, 1}));
	ASSERT_EQ(lay.levels.size(), 4u);
	// every forward edge goes down
	for (const auto& e: edges)
		if (!lay.backEdges.count({e.from, e.to})) EXPECT_LT(lay.topLeft.at(e.from).y, lay.topLeft.at(e.to).y);
	// no overlaps on a level
	for (const auto& lvl: lay.levels)
		for (size_t i = 1; i < lvl.size(); ++i)
			EXPECT_GE(lay.topLeft.at(lvl[i]).x - lay.topLeft.at(lvl[i - 1]).x, 100.f + o.nodeGap - 0.01f);
	EXPECT_FLOAT_EQ(lay.bounds.x, o.margin);
	EXPECT_FLOAT_EQ(lay.bounds.y, o.margin);
}

TEST(GraphLayoutTest, LayeredLayoutIsDeterministicAndHandlesSelfLoops)
{
	std::vector<uint64_t> nodes{3, 1, 2};
	std::vector<GPoint> sizes(3, GPoint{50.f, 20.f});
	const auto e = E({{3, 3}, {3, 1}, {1, 2}, {9, 1}}); // self loop and an unknown node are ignored
	const auto a = layeredLayout(nodes, sizes, e, std::nullopt);
	const auto b = layeredLayout(nodes, sizes, e, std::nullopt);
	for (uint64_t n: nodes)
		EXPECT_EQ(a.topLeft.at(n), b.topLeft.at(n));
	EXPECT_EQ(a.rank.at(3), 0);
	EXPECT_EQ(a.rank.at(2), 2);
}

// ── SCC, depth, neighbourhood ────────────────────────────────────────────────

TEST(GraphLayoutTest, TarjanScc)
{
	const auto adj = GraphAdjacency::build(E({{1, 2}, {2, 3}, {3, 1}, {3, 4}, {4, 5}, {5, 4}, {6, 6}}));
	auto sccs = stronglyConnectedComponents({1, 2, 3, 4, 5, 6, 7}, adj.succ);
	std::vector<std::vector<uint64_t>> norm;
	for (auto& s: sccs)
		norm.push_back(sorted(s));
	std::sort(norm.begin(), norm.end());
	ASSERT_EQ(norm.size(), 4u);
	EXPECT_EQ(norm[0], (std::vector<uint64_t>{1, 2, 3}));
	EXPECT_EQ(norm[1], (std::vector<uint64_t>{4, 5}));
	EXPECT_EQ(norm[2], (std::vector<uint64_t>{6}));
	EXPECT_EQ(norm[3], (std::vector<uint64_t>{7}));
}

TEST(GraphLayoutTest, TarjanSccDeepChainDoesNotOverflow)
{
	std::vector<uint64_t> nodes;
	std::vector<EdgeRef> edges;
	for (uint64_t i = 0; i < 100000; ++i)
	{
		nodes.push_back(i);
		if (i) edges.push_back({i - 1, i});
	}
	edges.push_back({99999, 0});
	const auto adj = GraphAdjacency::build(edges);
	const auto sccs = stronglyConnectedComponents(nodes, adj.succ);
	ASSERT_EQ(sccs.size(), 1u);
	EXPECT_EQ(sccs[0].size(), 100000u);
}

TEST(GraphLayoutTest, DepthFromRootsAndNeighbourhoods)
{
	const auto adj = GraphAdjacency::build(E({{1, 2}, {2, 3}, {3, 4}, {9, 3}}));
	const auto d = depthFromRoots({1, 2, 3, 4, 9}, adj);
	EXPECT_EQ(d.at(1), 0);
	EXPECT_EQ(d.at(9), 0);
	EXPECT_EQ(d.at(3), 1); // shortest from any root
	EXPECT_EQ(d.at(4), 2);
	const auto both = neighbourhood(3, 1, adj);
	EXPECT_EQ(sorted({both.begin(), both.end()}), (std::vector<uint64_t>{2, 3, 4, 9}));
	const auto callees = neighbourhood(1, 2, adj, NeighbourDirection::Successors);
	EXPECT_EQ(sorted({callees.begin(), callees.end()}), (std::vector<uint64_t>{1, 2, 3}));
	const auto callers = neighbourhood(4, 5, adj, NeighbourDirection::Predecessors);
	EXPECT_EQ(sorted({callers.begin(), callers.end()}), (std::vector<uint64_t>{1, 2, 3, 4, 9}));
	const auto zero = neighbourhood(3, 0, adj);
	EXPECT_EQ(zero.size(), 1u);
}

TEST(GraphLayoutTest, CyclicGraphDepthStartsAtFirstNode)
{
	const auto adj = GraphAdjacency::build(E({{1, 2}, {2, 1}}));
	const auto d = depthFromRoots({1, 2}, adj);
	EXPECT_EQ(d.at(1), 0);
	EXPECT_EQ(d.at(2), 1);
}

TEST(GraphLayoutTest, ForceDirectedPullsNeighboursTogether)
{
	std::vector<ForceNode> fn;
	for (uint64_t i = 0; i < 6; ++i)
		fn.push_back({i, float(i * 37 % 200) - 100.f, float(i * 91 % 200) - 100.f});
	// two triangles, no edges between them
	forceDirectedLayout(fn, E({{0, 1}, {1, 2}, {2, 0}, {3, 4}, {4, 5}, {5, 3}}), 60);
	auto dist = [&](int a, int b) { return std::hypot(fn[size_t(a)].x - fn[size_t(b)].x, fn[size_t(a)].y - fn[size_t(b)].y); };
	EXPECT_LT(dist(0, 1), dist(0, 4) + 1.f);
	for (int i = 0; i < 6; ++i)
		for (int j = i + 1; j < 6; ++j)
			EXPECT_GT(dist(i, j), 10.f);
}

TEST(GraphLayoutTest, ForceDirectedGridModeScales)
{
	std::vector<ForceNode> fn;
	std::vector<EdgeRef> edges;
	for (uint64_t i = 0; i < 4000; ++i)
	{
		fn.push_back({i, float((i * 7919) % 2000), float((i * 104729) % 2000)});
		if (i) edges.push_back({i / 2, i});
	}
	const auto t0 = std::chrono::steady_clock::now();
	forceDirectedLayout(fn, edges, 20, 400);
	EXPECT_LT(std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count(), 20.0);
	for (const auto& n: fn)
	{
		EXPECT_TRUE(std::isfinite(n.x));
		EXPECT_TRUE(std::isfinite(n.y));
	}
}

// ── Scene geometry ───────────────────────────────────────────────────────────

TEST(GraphSceneTest, FinalizeBoundsIndexAndHitTest)
{
	GraphScene s;
	s.nodes.push_back(SceneNode{.id = 7, .rect = {10, 10, 100, 50}});
	s.nodes.push_back(SceneNode{.id = 8, .rect = {50, 30, 100, 50}});
	SceneEdge e;
	e.from = 7;
	e.to = 8;
	e.points = {{60, 60}, {60, 100}, {300, 100}, {300, 200}};
	s.edges.push_back(e);
	s.finalize(40.f);
	EXPECT_EQ(s.findNode(8), 1);
	EXPECT_EQ(s.findNode(99), -1);
	EXPECT_EQ(s.nodeAt({60, 40}), 1); // topmost (last) wins
	EXPECT_EQ(s.nodeAt({15, 15}), 0);
	EXPECT_EQ(s.nodeAt({500, 500}), -1);
	s.nodes[1].visible = false;
	EXPECT_EQ(s.nodeAt({60, 40}), 0);
	EXPECT_EQ(s.visibleNodeCount(), 1);
	EXPECT_LE(s.bounds.x, 10.f - 40.f);
	EXPECT_GE(s.bounds.bottom(), 200.f + 40.f);
	EXPECT_EQ(s.edges[0].polyline.size(), 17u);
	EXPECT_EQ(s.edges[0].polyline.front(), (GPoint{60, 60}));
	EXPECT_EQ(s.edges[0].polyline.back(), (GPoint{300, 200}));
}

TEST(GraphSceneTest, DashesAndArrowHeads)
{
	const auto dashes = dashPolyline({{0, 0}, {100, 0}}, 10.f, 5.f);
	ASSERT_EQ(dashes.size(), 7u); // 0-10, 15-25, ... 90-100
	EXPECT_FLOAT_EQ(dashes[1].first.x, 15.f);
	EXPECT_FLOAT_EQ(dashes[1].second.x, 25.f);
	EXPECT_EQ(dashPolyline({{0, 0}, {10, 0}, {10, 10}}, 0.f, 0.f).size(), 2u); // solid
	const ArrowHead a = arrowHead({100, 100}, {100, 0}, 10.f, 5.f);
	EXPECT_EQ(a.tip, (GPoint{100, 100}));
	EXPECT_FLOAT_EQ(a.left.y, 90.f);
	EXPECT_FLOAT_EQ(std::abs(a.left.x - a.right.x), 10.f);
}

TEST(GraphSceneTest, ColourHelpers)
{
	const Rgba c = rgbaHex(0x89b4fa);
	EXPECT_EQ(rgbaR(c), 0x89);
	EXPECT_EQ(rgbaG(c), 0xb4);
	EXPECT_EQ(rgbaB(c), 0xfa);
	EXPECT_EQ(rgbaA(c), 0xff);
	EXPECT_EQ(rgbaA(rgbaWithAlpha(c, 40)), 40);
	EXPECT_EQ(rgbaA(rgbaFade(c, 0.5f)), 128);
	EXPECT_EQ(rgbaCss(c), "#89b4fa");
}

TEST(GraphSceneTest, ViewTransformFitZoomAndCentre)
{
	ViewTransform t = ViewTransform::fit({0, 0, 1000, 500}, 500, 500, 0.f);
	EXPECT_FLOAT_EQ(t.zoom, 0.5f);
	const GPoint c = t.toView({500, 250});
	EXPECT_FLOAT_EQ(c.x, 250.f);
	EXPECT_FLOAT_EQ(c.y, 250.f);
	// zoom about an anchor keeps the anchor's scene point
	const GPoint before = t.toScene({100, 120});
	t.zoomAt(1.18f, {100, 120});
	const GPoint after = t.toScene({100, 120});
	EXPECT_NEAR(before.x, after.x, 1e-3f);
	EXPECT_NEAR(before.y, after.y, 1e-3f);
	t.centerOn({10, 20}, 400, 300);
	const GPoint v = t.toView({10, 20});
	EXPECT_NEAR(v.x, 200.f, 1e-3f);
	EXPECT_NEAR(v.y, 150.f, 1e-3f);
	t.zoomAt(1e9f, {0, 0});
	EXPECT_FLOAT_EQ(t.zoom, ViewTransform::kMaxZoom);
	EXPECT_EQ(ViewTransform::fit({}, 100, 100).zoom, 1.f);
}

// ── Search ───────────────────────────────────────────────────────────────────

TEST(GraphSearchTest, GlobMatchSemantics)
{
	EXPECT_TRUE(graphGlobMatch("*pha*", "alpha"));
	EXPECT_TRUE(graphGlobMatch("AL*", "alpha"));
	EXPECT_FALSE(graphGlobMatch("pha", "alpha"));
	EXPECT_TRUE(graphGlobMatch("a?pha", "alpha"));
	EXPECT_TRUE(graphGlobMatch("[ab]lpha", "Alpha"));
	EXPECT_FALSE(graphGlobMatch("[!a]lpha", "alpha"));
	EXPECT_TRUE(graphGlobMatch("f[0-9]", "f7"));
	EXPECT_TRUE(graphGlobMatch("*", ""));
	EXPECT_FALSE(graphGlobMatch("?", ""));
	EXPECT_TRUE(graphGlobMatch("a[b", "a[b")); // unterminated class: literal
}

TEST(GraphSearchTest, NameFilterAndSceneSearch)
{
	EXPECT_TRUE(graphNameFilterMatches("", "x"));
	EXPECT_TRUE(graphNameFilterMatches("MAIN", "do_main_loop"));
	EXPECT_FALSE(graphNameFilterMatches("main*", "do_main_loop"));
	GraphScene s;
	s.nodes.push_back(SceneNode{.id = 0x401000, .searchText = "main"});
	s.nodes.push_back(SceneNode{.id = 0x402000, .searchText = "helper"});
	s.nodes.push_back(SceneNode{.id = 0x403000, .visible = false, .searchText = "main2"});
	s.finalize();
	EXPECT_EQ(graphSearchScene(s, "ma"), (std::vector<uint64_t>{0x401000}));
	EXPECT_EQ(graphSearchScene(s, "0x402"), (std::vector<uint64_t>{0x402000}));
	EXPECT_TRUE(graphSearchScene(s, "").empty());
}

// ── Export ───────────────────────────────────────────────────────────────────

TEST(GraphExportTest, SvgContainsItemsAndEscapes)
{
	GraphScene s;
	SceneNode n;
	n.id = 1;
	n.rect = {0, 0, 100, 40};
	n.headerFill = catppuccin::mantle;
	n.headerHeight = 20;
	n.texts.push_back(SceneText{"a<b> & \"c\"", {5, 0, 90, 20}});
	n.tooltip = "tip";
	s.nodes.push_back(n);
	SceneNode hidden = n;
	hidden.id = 2;
	hidden.visible = false;
	hidden.texts[0].text = "HIDDEN";
	s.nodes.push_back(hidden);
	SceneEdge e;
	e.points = {{50, 40}, {50, 80}, {50, 80}, {50, 120}};
	e.dashed = true;
	s.edges.push_back(e);
	s.regions.push_back(SceneRegion{.rect = {-10, -10, 120, 60}, .fill = rgbaHex(0x89b4fa, 25), .label = "Module 3"});
	s.finalize();
	const std::string svg = sceneToSvg(s, {.title = "T"});
	EXPECT_NE(svg.find("<svg xmlns"), std::string::npos);
	EXPECT_NE(svg.find("<title>T</title>"), std::string::npos);
	EXPECT_NE(svg.find("a&lt;b&gt; &amp; &quot;c&quot;"), std::string::npos);
	EXPECT_EQ(svg.find("HIDDEN"), std::string::npos);
	EXPECT_NE(svg.find(" C50,80 50,80 50,120"), std::string::npos);
	EXPECT_NE(svg.find("stroke-dasharray=\"6.4 4.8\""), std::string::npos); // 4/3 pen widths x 1.6
	EXPECT_NE(svg.find("Module 3"), std::string::npos);
	EXPECT_NE(svg.find("fill-opacity"), std::string::npos);
	EXPECT_NE(svg.find("</svg>"), std::string::npos);
	EXPECT_NE(sceneToSvg(s, {.includeHidden = true}).find("HIDDEN"), std::string::npos);
}

TEST(GraphExportTest, EscapingHelpers)
{
	EXPECT_EQ(graphDotEscape("a\"b\\c\nd"), "a\\\"b\\\\c\\nd");
	EXPECT_EQ(graphXmlEscape("<'&'>"), "&lt;&apos;&amp;&apos;&gt;");
}

// ── CFG algorithms ───────────────────────────────────────────────────────────

TEST(GraphCfgTest, NaturalLoopsNestedAndAnnotated)
{
	// 1 -> 2 -> 3 -> 4 -> 3 (inner), 4 -> 2 (outer), 2 -> 5
	std::vector<BasicBlockData> blocks;
	for (uint64_t i = 1; i <= 5; ++i)
		blocks.push_back(BasicBlockData{.id = i, .address = i * 0x10, .isEntry = i == 1});
	std::vector<CFGEdgeData> edges{{1, 2}, {2, 3}, {3, 4}, {4, 3}, {4, 2}, {2, 5}};
	const auto loops = detectNaturalLoops(blocks, edges);
	ASSERT_EQ(loops.size(), 2u);
	EXPECT_EQ(loops[0].header, 2u);
	EXPECT_EQ(loops[0].body, (std::vector<uint64_t>{2, 3, 4}));
	EXPECT_EQ(loops[1].header, 3u);
	EXPECT_EQ(loops[1].body, (std::vector<uint64_t>{3, 4}));
	EXPECT_EQ(annotateLoops(blocks, edges), 2);
	EXPECT_TRUE(blocks[1].isLoopHeader);
	EXPECT_TRUE(blocks[2].isLoopHeader);
	EXPECT_EQ(blocks[3].loopId, blocks[2].loopId); // innermost wins
	EXPECT_NE(blocks[1].loopId, blocks[2].loopId);
	EXPECT_EQ(blocks[4].loopId, -1);
	// data that already has loop ids is left alone
	EXPECT_EQ(annotateLoops(blocks, edges), 0);
}

TEST(GraphCfgTest, ChainCompressionKeepsBranchesAndRing)
{
	// entry 0 -> 1 -> 2 -> 3 (branch) -> 4 / 5 ; ring 10 -> 11 -> 10 unreachable
	std::vector<BasicBlockData> blocks;
	for (uint64_t i: {0, 1, 2, 3, 4, 5, 10, 11})
		blocks.push_back(BasicBlockData{.id = i, .address = i, .instrs = {BlockInstr{i, "nop"}}, .isEntry = i == 0});
	std::vector<CFGEdgeData> edges{{0, 1}, {1, 2}, {2, 3}, {3, 4}, {3, 5}, {10, 11}, {11, 10}};
	std::unordered_map<uint64_t, std::vector<uint64_t>> members;
	const auto c = compressChains(blocks, edges, &members);
	std::vector<uint64_t> ids;
	for (const auto& b: c.blocks)
		ids.push_back(b.id);
	EXPECT_EQ(sorted(ids), (std::vector<uint64_t>{0, 3, 4, 5, 10, 11}));
	EXPECT_EQ(members.at(0), (std::vector<uint64_t>{0, 1, 2}));
	EXPECT_TRUE(isChainSummary(c.blocks[0]));
	EXPECT_EQ(c.blocks[0].instrs[0].text, "[chain: 3 instructions]");
	bool headToBranch = false;
	for (const auto& e: c.edges)
		headToBranch = headToBranch || (e.from == 0 && e.to == 3);
	EXPECT_TRUE(headToBranch);
	EXPECT_EQ(cfgEntryBlock(c.blocks, c.edges), 0u);
}

TEST(GraphCfgTest, EntryFallbacks)
{
	std::vector<BasicBlockData> blocks{BasicBlockData{.id = 5}, BasicBlockData{.id = 6}};
	EXPECT_EQ(cfgEntryBlock(blocks, {{5, 6}}), 5u);
	EXPECT_EQ(cfgEntryBlock(blocks, {{6, 5}}), 6u);
	EXPECT_EQ(cfgEntryBlock(blocks, {{5, 6}, {6, 5}}), 5u);
	EXPECT_EQ(cfgEntryBlock({}, {}), 0u);
}

TEST(GraphCfgTest, ThousandsOfBlocksLayOut)
{
	// A wide CFG: 2000 diamonds in sequence, forced uncompressed.
	std::vector<BasicBlockData> blocks;
	std::vector<CFGEdgeData> edges;
	for (uint64_t i = 0; i < 2000; ++i)
	{
		const uint64_t a = i * 3, b = a + 1, c = a + 2;
		blocks.push_back(BasicBlockData{.id = a, .address = a, .instrs = {{a, "cmp"}}, .isEntry = i == 0});
		blocks.push_back(BasicBlockData{.id = b, .address = b, .instrs = {{b, "mov"}}});
		blocks.push_back(BasicBlockData{.id = c, .address = c, .instrs = {{c, "add"}}});
		edges.push_back({a, b, EdgeKind::TrueBranch});
		edges.push_back({a, c, EdgeKind::FalseBranch});
		if (i + 1 < 2000)
		{
			edges.push_back({b, a + 3});
			edges.push_back({c, a + 3});
		}
	}
	CfgLayoutOptions o;
	o.forceUncompressed = true;
	const auto t0 = std::chrono::steady_clock::now();
	const auto lay = layoutCfg(blocks, edges, o);
	EXPECT_LT(std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count(), 20.0);
	EXPECT_EQ(lay.nodeCount, 6000);
	EXPECT_EQ(lay.scene.edges.size(), edges.size());
}
