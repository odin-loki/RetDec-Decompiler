/**
 * @file tests/gui/cfg_panel_test.cpp
 * @brief CFG visualiser tests: port of the Qt cfg_panel_test plus headless FuseUI interaction tests.
 *
 *   - EdgeKind colour mapping (all 6 kinds) and dash styles
 *   - Block geometry (fixed width, taller with more instructions, header above footer)
 *   - CFG layout: empty, single node, linear chain, diamond, back edge, >200 blocks compression, loop regions
 *   - CfgPanel: construction, setCfg, clear, function label, block click -> onBlockActivated(address),
 *     context menu Go to Function / Copy Address, chain click expands, toolbar buttons, search, export
 *   - GraphView: fit / reset zoom / Ctrl+wheel / pan, minimap, tooltip
 *
 * @copyright (c) 2026 Odin Loch trading as Imortek
 */

#include "graph_test_util.h"

#include "retdec/gui/model/cfg_graph.h"
#include "retdec/gui/ui/cfg_panel.h"

#include <gtest/gtest.h>

#include <format>
#include <fstream>
#include <sstream>

using namespace retdec::gui;
using namespace retdec::gui::model;
using retdec::gui::test::Headless;
using retdec::gui::ui::CfgPanel;
using retdec::gui::ui::GraphExportFormat;
using retdec::gui::ui::GraphView;

namespace {

BasicBlockData makeBlock(uint64_t id, bool entry = false, bool exit = false, bool loopHeader = false, int loopId = -1)
{
	BasicBlockData b;
	b.id = id;
	b.address = id * 4;
	b.isEntry = entry;
	b.isExit = exit;
	b.isLoopHeader = loopHeader;
	b.loopId = loopId;
	for (int i = 0; i < 3; ++i)
		b.instrs.push_back(BlockInstr{b.address + uint64_t(i * 4), std::format("mov r{}, r{}", i, i + 1)});
	return b;
}

CFGEdgeData makeEdge(uint64_t from, uint64_t to, EdgeKind kind = EdgeKind::FallThrough)
{
	return CFGEdgeData{from, to, kind};
}

CfgLayout load(const std::vector<BasicBlockData>& b, const std::vector<CFGEdgeData>& e)
{
	return layoutCfg(b, e);
}

std::pair<std::vector<BasicBlockData>, std::vector<CFGEdgeData>> chain(int n)
{
	std::vector<BasicBlockData> blocks;
	std::vector<CFGEdgeData> edges;
	blocks.push_back(makeBlock(0, true));
	for (uint64_t i = 1; i < uint64_t(n); ++i)
	{
		blocks.push_back(makeBlock(i));
		edges.push_back(makeEdge(i - 1, i));
	}
	return {blocks, edges};
}

std::pair<std::vector<BasicBlockData>, std::vector<CFGEdgeData>> sampleCfg()
{
	// 1 -> 2 (loop header) -> 3 -> 2 (back) ; 2 -> 4 (false) -> 5 exit ; 1 -> 6 (true) -> 5
	std::vector<BasicBlockData> blocks = {
		makeBlock(1, true),
		makeBlock(2, false, false, true, 0),
		makeBlock(3, false, false, false, 0),
		makeBlock(4),
		makeBlock(5, false, true),
		makeBlock(6),
	};
	blocks[2].instrs.push_back({0x40, "add r1, 1"});
	blocks[2].instrs.push_back({0x44, "cmp r1, r2"});
	std::vector<CFGEdgeData> edges = {
		makeEdge(1, 2, EdgeKind::FalseBranch),
		makeEdge(1, 6, EdgeKind::TrueBranch),
		makeEdge(2, 3, EdgeKind::TrueBranch),
		makeEdge(3, 2, EdgeKind::BackEdge),
		makeEdge(2, 4, EdgeKind::FalseBranch),
		makeEdge(4, 5),
		makeEdge(6, 5, EdgeKind::ExceptionEdge),
		makeEdge(4, 4, EdgeKind::UnresolvedIndirect),
	};
	return {blocks, edges};
}

struct PanelFixture
{
	Headless h{1200.f, 800.f};
	CfgPanel panel{h.ui, h.ui.root()};
	PanelFixture()
	{
		h.ui.setCss(h.ui.root(), "padding", "0px");
		h.frame();
	}
};

} // namespace

// ═════════════════════════════════════════════════════════════════════════════
// EdgeKind colour tests
// ═════════════════════════════════════════════════════════════════════════════

TEST(CFGEdgeColorTest, FallThroughDistinct)
{
	EXPECT_NE(rgbaA(cfgEdgeColor(EdgeKind::FallThrough)), 0);
}

TEST(CFGEdgeColorTest, TrueBranchIsGreen)
{
	const Rgba c = cfgEdgeColor(EdgeKind::TrueBranch);
	EXPECT_GT(rgbaG(c), rgbaR(c));
	EXPECT_GT(rgbaG(c), rgbaB(c));
}

TEST(CFGEdgeColorTest, FalseBranchIsReddish)
{
	const Rgba c = cfgEdgeColor(EdgeKind::FalseBranch);
	EXPECT_GT(rgbaR(c), rgbaB(c));
}

TEST(CFGEdgeColorTest, AllKindsValid)
{
	for (auto kind:
		 {EdgeKind::FallThrough,
		  EdgeKind::TrueBranch,
		  EdgeKind::FalseBranch,
		  EdgeKind::BackEdge,
		  EdgeKind::ExceptionEdge,
		  EdgeKind::UnresolvedIndirect})
	{
		EXPECT_EQ(rgbaA(cfgEdgeColor(kind)), 255);
		EXPECT_STRNE(edgeKindName(kind), "");
	}
}

TEST(CFGEdgeColorTest, BackEdgeAndFalseBranchDiffer)
{
	EXPECT_NE(cfgEdgeColor(EdgeKind::BackEdge), cfgEdgeColor(EdgeKind::FalseBranch));
}

TEST(CFGEdgeColorTest, ExceptionEdgeIsPurplish)
{
	const Rgba c = cfgEdgeColor(EdgeKind::ExceptionEdge);
	EXPECT_GT(rgbaB(c), rgbaG(c));
	EXPECT_GT(rgbaR(c), rgbaG(c));
}

TEST(CFGEdgeColorTest, BackEdgeAndUnresolvedAreDashed)
{
	EXPECT_TRUE(cfgEdgeDashed(EdgeKind::BackEdge));
	EXPECT_TRUE(cfgEdgeDashed(EdgeKind::UnresolvedIndirect));
	EXPECT_FALSE(cfgEdgeDashed(EdgeKind::FallThrough));
	EXPECT_FALSE(cfgEdgeDashed(EdgeKind::TrueBranch));
	EXPECT_FALSE(cfgEdgeDashed(EdgeKind::FalseBranch));
	EXPECT_FALSE(cfgEdgeDashed(EdgeKind::ExceptionEdge));
}

// ═════════════════════════════════════════════════════════════════════════════
// Block geometry (Qt BasicBlockItem tests)
// ═════════════════════════════════════════════════════════════════════════════

TEST(CFGBlockItemTest, BoundingRectHasPositiveSize)
{
	const auto lay = load({makeBlock(1)}, {});
	ASSERT_EQ(lay.scene.nodes.size(), 1u);
	EXPECT_GT(lay.scene.nodes[0].rect.w, 0);
	EXPECT_GT(lay.scene.nodes[0].rect.h, 0);
}

TEST(CFGBlockItemTest, BoundingRectWidthIsFixed)
{
	auto b1 = makeBlock(1);
	auto b2 = makeBlock(2);
	b2.instrs.clear();
	const auto lay = load({b1, b2}, {});
	ASSERT_EQ(lay.scene.nodes.size(), 2u);
	EXPECT_FLOAT_EQ(lay.scene.nodes[0].rect.w, lay.scene.nodes[1].rect.w);
	EXPECT_FLOAT_EQ(lay.scene.nodes[0].rect.w, cfg::kBlockWidth);
}

TEST(CFGBlockItemTest, MoreInstructionsMakeTaller)
{
	auto b1 = makeBlock(1);
	auto b2 = makeBlock(2);
	for (int i = 0; i < 6; ++i)
		b2.instrs.push_back(BlockInstr{0, "nop"});
	EXPECT_GT(cfgBlockHeight(b2), cfgBlockHeight(b1));
	// truncated: header + 3 previews + "more" line + footer
	EXPECT_FLOAT_EQ(cfgBlockHeight(b2), cfg::kHeaderH + 4 * cfg::kInstrH + cfg::kFooterH);
}

TEST(CFGBlockItemTest, BlockIdReturned)
{
	const auto lay = load({makeBlock(42)}, {});
	EXPECT_EQ(lay.scene.nodes[0].id, 42u);
}

TEST(CFGBlockItemTest, BlockAddressReturned)
{
	auto b = makeBlock(5);
	const auto lay = load({b}, {});
	EXPECT_EQ(lay.addressOf.at(5), b.address);
}

TEST(CFGBlockItemTest, TopCentreAboveBottomCentre)
{
	const auto lay = load({makeBlock(1)}, {});
	const GRect r = lay.scene.nodes[0].rect;
	EXPECT_LT(r.y, r.bottom());
}

TEST(CFGBlockItemTest, HeaderShowsAddressAndTag)
{
	EXPECT_EQ(cfgBlockHeaderText(makeBlock(1, true)), "0x0000000000000004  [entry]");
	EXPECT_NE(cfgBlockHeaderText(makeBlock(1, false, true)).find("[exit]"), std::string::npos);
	EXPECT_NE(cfgBlockHeaderText(makeBlock(1, false, false, true)).find("[loop header]"), std::string::npos);
	auto big = makeBlock(1);
	for (int i = 0; i < 4; ++i)
		big.instrs.push_back({0, "nop"});
	const auto lay = load({big}, {});
	const auto& texts = lay.scene.nodes[0].texts;
	ASSERT_EQ(texts.size(), 5u); // header, 3 previews, "… 4 more"
	EXPECT_NE(texts.back().text.find("4 more"), std::string::npos);
}

TEST(CFGBlockItemTest, NodeColouringByKind)
{
	const auto lay = load(
		{makeBlock(1, true), makeBlock(2, false, true), makeBlock(3, false, false, true, 0), makeBlock(4)},
		{makeEdge(1, 3), makeEdge(3, 4), makeEdge(4, 3), makeEdge(3, 2)});
	EXPECT_EQ(lay.scene.node(1)->border, catppuccin::green);
	EXPECT_EQ(lay.scene.node(2)->border, catppuccin::red);
	EXPECT_EQ(lay.scene.node(3)->border, catppuccin::blue);
	EXPECT_EQ(lay.scene.node(4)->border, catppuccin::surface1);
	EXPECT_EQ(lay.scene.node(4)->headerFill, catppuccin::mantle);
}

// ═════════════════════════════════════════════════════════════════════════════
// CFG layout (Qt CFGScene tests)
// ═════════════════════════════════════════════════════════════════════════════

TEST(CFGSceneTest, EmptyCFGDoesNotCrash)
{
	const auto lay = load({}, {});
	EXPECT_EQ(lay.nodeCount, 0);
	EXPECT_TRUE(lay.scene.empty());
}

TEST(CFGSceneTest, SingleNodeGraphAdded)
{
	const auto lay = load({makeBlock(1, true)}, {});
	EXPECT_EQ(lay.nodeCount, 1);
	EXPECT_FALSE(lay.compressed);
}

TEST(CFGSceneTest, LinearChainThreeNodes)
{
	const auto lay =
		load({makeBlock(1, true), makeBlock(2), makeBlock(3, false, true)}, {makeEdge(1, 2), makeEdge(2, 3)});
	EXPECT_EQ(lay.nodeCount, 3);
	// one block per level, top to bottom
	EXPECT_LT(lay.scene.node(1)->rect.bottom(), lay.scene.node(2)->rect.y);
	EXPECT_LT(lay.scene.node(2)->rect.bottom(), lay.scene.node(3)->rect.y);
	// Qt spacing: level tops 140 apart for full blocks
	EXPECT_FLOAT_EQ(lay.scene.node(2)->rect.y - lay.scene.node(1)->rect.y, cfg::kLevelH);
}

TEST(CFGSceneTest, DiamondBranchFourNodes)
{
	const auto lay = load(
		{makeBlock(1, true), makeBlock(2), makeBlock(3), makeBlock(4, false, true)},
		{
			makeEdge(1, 2, EdgeKind::TrueBranch),
			makeEdge(1, 3, EdgeKind::FalseBranch),
			makeEdge(2, 4),
			makeEdge(3, 4),
		});
	EXPECT_EQ(lay.nodeCount, 4);
	const GRect r2 = lay.scene.node(2)->rect, r3 = lay.scene.node(3)->rect;
	EXPECT_FLOAT_EQ(r2.y, r3.y);                                 // same level
	EXPECT_GE(std::abs(r2.x - r3.x), cfg::kNodeSpacingH - 0.5f); // Qt horizontal spacing
	// entry centred over its two successors, exit under them
	EXPECT_NEAR(lay.scene.node(1)->rect.center().x, (r2.center().x + r3.center().x) / 2, 1.f);
	EXPECT_NEAR(lay.scene.node(4)->rect.center().x, (r2.center().x + r3.center().x) / 2, 1.f);
	ASSERT_EQ(lay.scene.edges.size(), 4u);
	for (const auto& e: lay.scene.edges)
		if (e.from == 1 && e.to == 2)
			EXPECT_EQ(e.color, catppuccin::green);
		else if (e.from == 1 && e.to == 3)
			EXPECT_EQ(e.color, catppuccin::red);
}

TEST(CFGSceneTest, BackEdgeGraphDoesNotCrash)
{
	const auto lay = load(
		{makeBlock(1, true), makeBlock(2, false, false, true, 0), makeBlock(3, false, false, false, 0)},
		{makeEdge(1, 2), makeEdge(2, 3), makeEdge(3, 2, EdgeKind::BackEdge)});
	EXPECT_EQ(lay.nodeCount, 3);
	EXPECT_TRUE(lay.backEdges.count({3, 2}));
	const SceneEdge* back = nullptr;
	for (const auto& e: lay.scene.edges)
		if (e.from == 3 && e.to == 2) back = &e;
	ASSERT_NE(back, nullptr);
	EXPECT_TRUE(back->dashed);
	EXPECT_EQ(back->color, catppuccin::peach);
	// arcs to the left of both blocks
	float minX = 1e9f;
	for (const auto& p: back->polyline)
		minX = std::min(minX, p.x);
	EXPECT_LT(minX, std::min(lay.scene.node(2)->rect.x, lay.scene.node(3)->rect.x));
}

TEST(CFGSceneTest, DfsBackEdgeDrawnAsBackEdgeEvenIfNotMarked)
{
	const auto lay =
		load({makeBlock(1, true), makeBlock(2), makeBlock(3)}, {makeEdge(1, 2), makeEdge(2, 3), makeEdge(3, 2)});
	for (const auto& e: lay.scene.edges)
		if (e.from == 3 && e.to == 2)
		{
			EXPECT_EQ(e.kind, int(EdgeKind::BackEdge));
			EXPECT_TRUE(e.dashed);
		}
}

TEST(CFGSceneTest, ClearGraphResetsNodeCount)
{
	CfgLayout lay = load({makeBlock(1, true), makeBlock(2)}, {makeEdge(1, 2)});
	EXPECT_EQ(lay.nodeCount, 2);
	lay = load({}, {});
	EXPECT_EQ(lay.nodeCount, 0);
}

TEST(CFGSceneTest, ReloadReplacesOldGraph)
{
	CfgLayout lay = load({makeBlock(1, true)}, {});
	lay = load({makeBlock(10, true), makeBlock(11), makeBlock(12)}, {makeEdge(10, 11), makeEdge(11, 12)});
	EXPECT_EQ(lay.nodeCount, 3);
}

TEST(CFGSceneTest, LargeGraphTriggersCompression)
{
	auto [blocks, edges] = chain(210);
	const auto lay = load(blocks, edges);
	EXPECT_TRUE(lay.compressed);
	EXPECT_LT(lay.nodeCount, 210);
	EXPECT_GE(lay.nodeCount, 1);
	// The chain collapses into the entry's summary node; the last block (no successor) stays.
	ASSERT_EQ(lay.nodeCount, 2);
	EXPECT_EQ(lay.scene.nodes[0].kind, cfg::Chain);
	EXPECT_NE(lay.scene.nodes[0].texts[1].text.find("[chain: 627 instructions]"), std::string::npos);
	EXPECT_EQ(lay.chainMembers.at(0).size(), 209u);
	EXPECT_EQ(lay.scene.nodes[1].kind, cfg::Block);
	ASSERT_EQ(lay.scene.edges.size(), 1u); // chain head -> last block
	EXPECT_EQ(lay.scene.edges[0].from, 0u);
	EXPECT_EQ(lay.scene.edges[0].to, 209u);
}

TEST(CFGSceneTest, ForceUncompressedShowsEveryBlock)
{
	auto [blocks, edges] = chain(210);
	CfgLayoutOptions o;
	o.forceUncompressed = true;
	const auto lay = layoutCfg(blocks, edges, o);
	EXPECT_FALSE(lay.compressed);
	EXPECT_EQ(lay.nodeCount, 210);
}

TEST(CFGSceneTest, SmallGraphNotCompressed)
{
	auto [blocks, edges] = chain(10);
	const auto lay = load(blocks, edges);
	EXPECT_FALSE(lay.compressed);
	EXPECT_EQ(lay.nodeCount, 10);
}

TEST(CFGSceneTest, LoopRegionBlocksHaveLoopId)
{
	std::vector<BasicBlockData> blocks = {
		makeBlock(1, true),
		makeBlock(2, false, false, true, 0),
		makeBlock(3, false, false, false, 0),
		makeBlock(4, false, false, true, 1),
		makeBlock(5, false, false, false, 1),
		makeBlock(6, false, true),
	};
	std::vector<CFGEdgeData> edges = {
		makeEdge(1, 2),
		makeEdge(2, 3),
		makeEdge(3, 2, EdgeKind::BackEdge),
		makeEdge(3, 4),
		makeEdge(4, 5),
		makeEdge(5, 4, EdgeKind::BackEdge),
		makeEdge(5, 6),
	};
	CfgLayout lay;
	EXPECT_NO_THROW(lay = load(blocks, edges));
	EXPECT_EQ(lay.nodeCount, 6);
	ASSERT_EQ(lay.scene.regions.size(), 2u);
	// region 0 encloses blocks 2 and 3, coloured blue then mauve (Qt palette order)
	const GRect r0 = lay.scene.regions[0].rect;
	EXPECT_LE(r0.x, lay.scene.node(2)->rect.x);
	EXPECT_GE(r0.bottom(), lay.scene.node(3)->rect.bottom());
	EXPECT_EQ(lay.scene.regions[0].border, rgbaWithAlpha(catppuccin::blue, 140));
	EXPECT_EQ(lay.scene.regions[1].border, rgbaWithAlpha(catppuccin::mauve, 140));
}

TEST(CFGSceneTest, LoopsDetectedWhenDataHasNone)
{
	auto lay = load(
		{makeBlock(1, true), makeBlock(2), makeBlock(3), makeBlock(4, false, true)},
		{makeEdge(1, 2), makeEdge(2, 3), makeEdge(3, 2), makeEdge(2, 4)});
	ASSERT_EQ(lay.loops.size(), 1u);
	EXPECT_EQ(lay.loops[0].header, 2u);
	EXPECT_EQ(lay.loops[0].body, (std::vector<uint64_t>{2, 3}));
	EXPECT_EQ(lay.scene.regions.size(), 1u);
	EXPECT_EQ(lay.scene.node(2)->border, catppuccin::blue); // detected loop header
}

TEST(CFGSceneTest, PortsSpreadForBranches)
{
	const auto lay = load(
		{makeBlock(1, true), makeBlock(2), makeBlock(3)},
		{makeEdge(1, 2, EdgeKind::TrueBranch), makeEdge(1, 3, EdgeKind::FalseBranch)});
	ASSERT_EQ(lay.scene.edges.size(), 2u);
	EXPECT_NE(lay.scene.edges[0].points.front().x, lay.scene.edges[1].points.front().x);
	for (const auto& e: lay.scene.edges)
	{
		EXPECT_FLOAT_EQ(e.points.front().y, lay.scene.node(1)->rect.bottom());
		EXPECT_FLOAT_EQ(e.points.back().y, lay.scene.node(e.to)->rect.y);
	}
}

TEST(CFGSceneTest, DotExportListsBlocksAndStyledEdges)
{
	auto [blocks, edges] = sampleCfg();
	const std::string dot = cfgToDot(blocks, edges, "main");
	EXPECT_NE(dot.find("digraph \"main\""), std::string::npos);
	EXPECT_NE(dot.find("\"0x1\" -> \"0x2\""), std::string::npos);
	EXPECT_NE(dot.find("style=dashed"), std::string::npos);
	EXPECT_NE(dot.find("mov r0, r1\\l"), std::string::npos);
	EXPECT_NE(dot.find("#a6e3a1"), std::string::npos); // true branch green
}

// ═════════════════════════════════════════════════════════════════════════════
// CfgPanel (Qt CFGPanel tests + headless interaction)
// ═════════════════════════════════════════════════════════════════════════════

TEST(CFGPanelTest, ConstructsWithoutCrash)
{
	Headless h;
	EXPECT_NO_THROW({ CfgPanel panel(h.ui, h.ui.root()); });
	h.frame();
}

TEST(CFGPanelTest, LoadCFGPopulatesScene)
{
	PanelFixture f;
	f.panel.setCfg({makeBlock(1, true), makeBlock(2)}, {makeEdge(1, 2)});
	f.h.frame();
	EXPECT_NE(f.h.ui.rect(f.panel.root()).w, 0);
	EXPECT_EQ(f.panel.nodeCount(), 2);
	EXPECT_FALSE(f.panel.emptyStateVisible());
	EXPECT_EQ(f.panel.infoLabel(), "2 blocks");
	EXPECT_TRUE(retdec::gui::ui::graphShown(f.h.ui, f.panel.view().node()));
}

TEST(CFGPanelTest, ClearResetsToEmpty)
{
	PanelFixture f;
	f.panel.setCfg({makeBlock(1, true)}, {});
	EXPECT_NO_THROW(f.panel.clear());
	f.h.frame();
	EXPECT_TRUE(f.panel.emptyStateVisible());
	EXPECT_EQ(f.panel.functionLabel(), "No function selected");
	EXPECT_EQ(f.panel.infoLabel(), "");
	EXPECT_FALSE(retdec::gui::ui::graphShown(f.h.ui, f.panel.view().node()));
	EXPECT_TRUE(f.h.findText("No control-flow graph").valid());
}

TEST(CFGPanelTest, FunctionSelectedUpdatesLabel)
{
	PanelFixture f;
	EXPECT_NO_THROW(f.panel.setFunction(0x1000, "my_function"));
	f.h.frame();
	EXPECT_EQ(f.panel.functionLabel(), "my_function  0x1000");
	EXPECT_TRUE(f.h.findText("my_function  0x1000").valid());
}

TEST(CFGPanelTest, BlockNavigationSignalOnClick)
{
	PanelFixture f;
	std::vector<uint64_t> got;
	f.panel.onBlockActivated = [&](uint64_t a) { got.push_back(a); };
	auto b = makeBlock(0xDEAD, true);
	b.address = 0xDEAD * 4;
	f.panel.setCfg({b}, {});
	f.h.frame();
	const auto c = f.panel.view().nodeCenter(0xDEAD);
	ASSERT_TRUE(c.has_value());
	f.h.click(*c);
	ASSERT_EQ(got.size(), 1u);
	EXPECT_EQ(got[0], 0xDEAD * 4u);
	EXPECT_EQ(f.panel.view().selectedNode(), std::optional<uint64_t>(0xDEAD));
	// Clicking empty canvas clears the selection and fires nothing.
	const auto vr = f.panel.view().viewportRect();
	f.h.click({vr.x + 5.f, vr.bottom() - 5.f});
	EXPECT_EQ(got.size(), 1u);
	EXPECT_FALSE(f.panel.view().selectedNode().has_value());
}

TEST(CFGPanelTest, ReloadCFGDoesNotCrash)
{
	PanelFixture f;
	f.panel.setCfg({makeBlock(1, true)}, {});
	f.panel.setCfg({makeBlock(10, true), makeBlock(11)}, {makeEdge(10, 11)});
	f.h.frame();
	EXPECT_EQ(f.panel.nodeCount(), 2);
}

TEST(CFGPanelTest, EmptyCFGLoadDoesNotCrash)
{
	PanelFixture f;
	EXPECT_NO_THROW(f.panel.setCfg({}, {}));
	f.h.frame();
	EXPECT_TRUE(f.panel.emptyStateVisible());
}

TEST(CFGPanelTest, ContextMenuGoToFunctionAndCopyAddress)
{
	PanelFixture f;
	uint64_t navigated = 0;
	f.panel.onAddressNavigated = [&](uint64_t a) { navigated = a; };
	f.panel.setFunction(0x401000, "main");
	f.panel.setCfg({makeBlock(1, true), makeBlock(2)}, {makeEdge(1, 2)});
	f.h.frame();
	const auto c = f.panel.view().nodeCenter(2);
	ASSERT_TRUE(c);
	f.h.click(*c, fuse::ui::MouseButton::Right);
	const auto item = f.h.findText("Go to Function", fuse::ui::widgets::overlayLayer(f.h.ui));
	ASSERT_TRUE(item.valid());
	f.h.clickNode(item);
	f.h.frame();
	EXPECT_EQ(navigated, 0x401000u);

	f.h.click(*c, fuse::ui::MouseButton::Right);
	const auto copy = f.h.findText("Copy Address", fuse::ui::widgets::overlayLayer(f.h.ui));
	ASSERT_TRUE(copy.valid());
	f.h.clickNode(copy);
	std::string clip;
	ASSERT_TRUE(f.h.ui.clipboardText(clip));
	EXPECT_EQ(clip, "0x8");
	EXPECT_EQ(f.panel.blockMenuItems(2).size(), 2u);
}

TEST(CFGPanelTest, ChainClickExpandsCompressedGraph)
{
	PanelFixture f;
	auto [blocks, edges] = chain(210);
	f.panel.setCfg(blocks, edges);
	f.h.frame();
	EXPECT_TRUE(f.panel.isCompressed());
	EXPECT_EQ(f.panel.infoLabel(), "210 blocks (compressed)");
	EXPECT_EQ(f.panel.expandChainsLabel(), "Expand chains");
	EXPECT_EQ(f.panel.blockMenuItems(0).size(), 3u);
	const auto c = f.panel.view().nodeCenter(0);
	ASSERT_TRUE(c);
	uint64_t clicked = ~0ull;
	f.panel.onBlockActivated = [&](uint64_t a) { clicked = a; };
	f.h.click(*c);
	f.h.frame(); // the expansion is posted, as Qt queued it
	EXPECT_EQ(clicked, 0u);
	EXPECT_FALSE(f.panel.isCompressed());
	EXPECT_EQ(f.panel.nodeCount(), 210);
	EXPECT_EQ(f.panel.expandChainsLabel(), "Compress chains");
	// toolbar button compresses again
	f.h.clickNode(f.panel.expandChainsButton());
	EXPECT_TRUE(f.panel.isCompressed());
	EXPECT_EQ(f.h.ui.text(f.panel.expandChainsButton()), "Expand chains");
}

TEST(CFGPanelTest, ToolbarFitAndResetZoom)
{
	PanelFixture f;
	auto [blocks, edges] = chain(30);
	f.panel.setCfg(blocks, edges);
	f.h.frame();
	GraphView& v = f.panel.view();
	const float fitted = v.zoom();
	EXPECT_LT(fitted, 1.f); // 30 levels do not fit at 100 %
	f.h.clickNode(f.panel.resetButton());
	EXPECT_FLOAT_EQ(v.zoom(), 1.f);
	f.h.clickNode(f.panel.fitButton());
	EXPECT_FLOAT_EQ(v.zoom(), fitted);
}

TEST(CFGPanelTest, SearchHighlightsAndCyclesMatches)
{
	PanelFixture f;
	auto [blocks, edges] = sampleCfg();
	f.panel.setCfg(blocks, edges);
	f.h.frame();
	f.h.ui.setFocus(f.panel.searchBox());
	f.h.type("cmp");
	EXPECT_EQ(f.panel.view().highlighted().size(), 1u);
	EXPECT_EQ(f.panel.view().selectedNode(), std::optional<uint64_t>(3));
	EXPECT_EQ(f.panel.search("mov r0"), 6);
	EXPECT_EQ(f.panel.nextMatch(), std::optional<uint64_t>(2));
	EXPECT_EQ(f.panel.search("0x14"), 1); // block 5 by address
	EXPECT_EQ(f.panel.search(""), 0);
	EXPECT_TRUE(f.panel.view().highlighted().empty());
}

TEST(CFGPanelTest, ExportTextFormats)
{
	PanelFixture f;
	auto [blocks, edges] = sampleCfg();
	f.panel.setFunction(0x1000, "loop_fn");
	f.panel.setCfg(blocks, edges);
	const std::string svg = f.panel.exportSvg();
	EXPECT_NE(svg.find("<svg"), std::string::npos);
	EXPECT_NE(svg.find("RetDec CFG Export"), std::string::npos);
	EXPECT_NE(svg.find("stroke-dasharray"), std::string::npos); // dashed back edge
	EXPECT_NE(svg.find("[loop header]"), std::string::npos);
	EXPECT_NE(f.panel.exportDot().find("digraph \"loop_fn\""), std::string::npos);

	GraphExportFormat requested = GraphExportFormat::Png;
	f.panel.onExportRequested = [&](GraphExportFormat x) { requested = x; };
	f.h.frame();
	f.h.clickNode(f.panel.button(GraphExportFormat::Svg));
	EXPECT_EQ(requested, GraphExportFormat::Svg);
	f.h.clickNode(f.panel.button(GraphExportFormat::Dot));
	EXPECT_EQ(requested, GraphExportFormat::Dot);

	const auto dotPath = retdec::gui::test::shotPath("retdec_cfg_export_test.dot");
	ASSERT_TRUE(f.panel.exportTo(GraphExportFormat::Dot, dotPath).has_value());
	{
		// Scoped: Windows cannot remove a file that still has an open handle.
		std::ifstream in(dotPath, std::ios::binary);
		std::stringstream ss;
		ss << in.rdbuf();
		EXPECT_EQ(ss.str(), f.panel.exportDot());
	}
	std::filesystem::remove(dotPath);
}

TEST(CFGPanelTest, ExportButtonOpensSaveDialogWithoutHost)
{
	PanelFixture f;
	f.panel.setCfg({makeBlock(1, true)}, {});
	f.h.frame();
	f.h.clickNode(f.panel.button(GraphExportFormat::Png));
	EXPECT_TRUE(f.h.findText("Export CFG").valid());
}

TEST(CFGPanelTest, ExportPngRendersScene)
{
	PanelFixture f;
	auto [blocks, edges] = sampleCfg();
	f.panel.setCfg(blocks, edges);
	f.h.frame();
	const fuse::ui::Image img = f.panel.view().renderImage(1.f);
	ASSERT_GT(img.width, 200);
	ASSERT_GT(img.height, 200);
	// background is Catppuccin base; some pixel differs (blocks drawn)
	const uint32_t bg = img.at(1, 1);
	bool drawn = false;
	for (int y = 0; y < img.height && !drawn; y += 3)
		for (int x = 0; x < img.width && !drawn; x += 3)
			drawn = img.at(x, y) != bg;
	EXPECT_TRUE(drawn);
	const auto path = retdec::gui::test::shotPath("retdec_cfg_export.png");
	EXPECT_TRUE(f.panel.exportPng(path).has_value());
	EXPECT_TRUE(std::filesystem::exists(path));
}

TEST(CFGPanelTest, RenderPanelScreenshot)
{
	PanelFixture f;
	auto [blocks, edges] = sampleCfg();
	f.panel.setFunction(0x401000, "loop_fn");
	f.panel.setCfg(blocks, edges);
	f.h.frame();
	if (const auto c = f.panel.view().nodeCenter(3)) f.h.ui.pointerMove(*c);
	f.h.frame(0.7f); // tooltip delay
	f.h.frame();
	EXPECT_TRUE(f.panel.view().tooltipVisible());
	EXPECT_NE(f.panel.view().tooltipText().find("cmp r1, r2"), std::string::npos);
	const fuse::ui::Image img = f.h.render();
	EXPECT_TRUE(fuse::app::writePng(img, retdec::gui::test::shotPath("retdec_cfg_panel.png").string()));
}

// ═════════════════════════════════════════════════════════════════════════════
// GraphView (Qt CFGView / MiniMapView tests)
// ═════════════════════════════════════════════════════════════════════════════

TEST(CFGViewTest, FitGraphDoesNotCrash)
{
	Headless h;
	GraphView view(h.ui, h.ui.root());
	GraphScene s;
	s.nodes.push_back(SceneNode{.id = 1, .rect = {0, 0, 100, 100}});
	s.finalize();
	view.setScene(s);
	EXPECT_NO_THROW(view.fitGraph());
	h.frame();
	EXPECT_GT(view.zoom(), 1.f); // a 100 px box fitted into a big view
}

TEST(CFGViewTest, ResetZoomDoesNotCrash)
{
	Headless h;
	GraphView view(h.ui, h.ui.root());
	EXPECT_NO_THROW(view.resetZoom());
	h.frame();
	EXPECT_FLOAT_EQ(view.zoom(), 1.f);
}

TEST(CFGViewTest, FitGraphOnEmptyScene)
{
	Headless h;
	GraphView view(h.ui, h.ui.root());
	EXPECT_NO_THROW(view.fitGraph());
	h.frame();
}

TEST(CFGViewTest, CtrlWheelZoomsAboutPointerAndWheelScrolls)
{
	PanelFixture f;
	auto [blocks, edges] = chain(8);
	f.panel.setCfg(blocks, edges);
	f.h.frame();
	GraphView& v = f.panel.view();
	const float z0 = v.zoom();
	const auto c = *v.nodeCenter(3);
	const GPoint before = v.contextToScene(c);
	f.h.ui.setModifiers(fuse::ui::kModCtrl);
	f.h.ui.wheel(c, {0.f, 1.f});
	f.h.ui.setModifiers(0);
	f.h.frame();
	EXPECT_NEAR(v.zoom(), z0 * 1.18f, 1e-4f);
	const GPoint after = v.contextToScene(c);
	EXPECT_NEAR(before.x, after.x, 0.01f);
	EXPECT_NEAR(before.y, after.y, 0.01f);
	// plain wheel scrolls (zoom unchanged)
	const float oy = v.transform().origin.y;
	f.h.ui.wheel(c, {0.f, -1.f});
	f.h.frame();
	EXPECT_NEAR(v.zoom(), z0 * 1.18f, 1e-4f);
	EXPECT_GT(v.transform().origin.y, oy);
}

TEST(CFGViewTest, KeyboardShortcutsFitAndReset)
{
	PanelFixture f;
	auto [blocks, edges] = chain(30);
	f.panel.setCfg(blocks, edges);
	f.h.frame();
	GraphView& v = f.panel.view();
	f.h.ui.setFocus(v.node());
	const float fitted = v.zoom();
	f.h.key(fuse::ui::Key::Num0, fuse::ui::kModCtrl);
	EXPECT_FLOAT_EQ(v.zoom(), 1.f);
	f.h.key(fuse::ui::Key::F, fuse::ui::kModCtrl);
	EXPECT_FLOAT_EQ(v.zoom(), fitted);
}

TEST(CFGViewTest, MiddleDragPans)
{
	PanelFixture f;
	f.panel.setCfg({makeBlock(1, true), makeBlock(2)}, {makeEdge(1, 2)});
	f.h.frame();
	GraphView& v = f.panel.view();
	const auto o = v.transform().origin;
	const auto vr = v.viewportRect();
	const fuse::ui::Vec2 p{vr.x + 20.f, vr.bottom() - 20.f};
	f.h.ui.pointerMove(p);
	f.h.ui.pointerDown(p, fuse::ui::MouseButton::Middle);
	f.h.ui.pointerMove({p.x + 50.f, p.y - 30.f});
	f.h.ui.pointerUp({p.x + 50.f, p.y - 30.f}, fuse::ui::MouseButton::Middle);
	f.h.frame();
	EXPECT_NEAR(v.transform().origin.x, o.x - 50.f / v.zoom(), 0.01f);
	EXPECT_NEAR(v.transform().origin.y, o.y + 30.f / v.zoom(), 0.01f);
}

TEST(MiniMapViewTest, ConstructsWithoutCrash)
{
	Headless h;
	EXPECT_NO_THROW({ GraphView v(h.ui, h.ui.root()); });
}

TEST(MiniMapViewTest, FixedSize)
{
	PanelFixture f;
	f.panel.setCfg({makeBlock(1, true)}, {});
	f.h.frame();
	const auto mm = f.panel.view().minimapRect();
	EXPECT_FLOAT_EQ(mm.w, 160.f);
	EXPECT_FLOAT_EQ(mm.h, 120.f);
	// top-right corner of the canvas
	const auto vr = f.panel.view().viewportRect();
	EXPECT_FLOAT_EQ(mm.right(), vr.right() - 8.f);
	EXPECT_FLOAT_EQ(mm.y, vr.y + 8.f);
}

TEST(MiniMapViewTest, SyncViewportDoesNotCrash)
{
	PanelFixture f;
	auto [blocks, edges] = chain(40);
	f.panel.setCfg(blocks, edges);
	f.h.frame();
	GraphView& v = f.panel.view();
	v.resetZoom();
	f.h.frame();
	// clicking the minimap's bottom centres the view on the end of the chain
	const auto mm = v.minimapRect();
	ASSERT_FALSE(mm.empty());
	const float yBefore = v.transform().origin.y;
	f.h.click({mm.x + mm.w * 0.5f, mm.bottom() - 6.f});
	EXPECT_GT(v.transform().origin.y, yBefore + 1000.f);
	f.panel.setMinimapVisible(false);
	f.h.frame();
	EXPECT_TRUE(v.minimapRect().empty());
}

// ═════════════════════════════════════════════════════════════════════════════
// Data model sanity
// ═════════════════════════════════════════════════════════════════════════════

TEST(CFGDataModelTest, BasicBlockDataDefaults)
{
	BasicBlockData b;
	EXPECT_EQ(b.id, 0u);
	EXPECT_EQ(b.address, 0u);
	EXPECT_FALSE(b.isEntry);
	EXPECT_FALSE(b.isExit);
	EXPECT_FALSE(b.isLoopHeader);
	EXPECT_EQ(b.loopId, -1);
	EXPECT_TRUE(b.instrs.empty());
}

TEST(CFGDataModelTest, CFGEdgeDataDefaults)
{
	CFGEdgeData e;
	EXPECT_EQ(e.from, 0u);
	EXPECT_EQ(e.to, 0u);
	EXPECT_EQ(e.kind, EdgeKind::FallThrough);
}

TEST(CFGDataModelTest, BlockInstrDefaults)
{
	BlockInstr i;
	EXPECT_EQ(i.address, 0u);
	EXPECT_TRUE(i.text.empty());
}

TEST(CFGDataModelTest, AllEdgeKindsDistinctColors)
{
	std::vector<Rgba> colors;
	for (auto k:
		 {EdgeKind::FallThrough,
		  EdgeKind::TrueBranch,
		  EdgeKind::FalseBranch,
		  EdgeKind::BackEdge,
		  EdgeKind::ExceptionEdge,
		  EdgeKind::UnresolvedIndirect})
		colors.push_back(cfgEdgeColor(k));
	EXPECT_NE(colors[1], colors[2]);
	EXPECT_NE(colors[3], colors[0]);
	EXPECT_NE(colors[4], colors[0]);
}
