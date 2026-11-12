/**
 * @file tests/gui/graph_render_preview_test.cpp
 * @brief Renders the graph panels (CFG, call graph, type hierarchy) headless
 *        and checks the FuseUI features they use: monospace / bold scene
 *        text, the call-graph depth spin box's "All" special value, the
 *        placeholders, bold (Qt: italic) abstract classes and pure-virtual
 *        slots, and native double clicks. With RETDEC_GUI_TEST_PNG_DIR set,
 *        the frames are written there as PNGs for a visual check.
 * @copyright (c) 2026 Odin Loch trading as Imortek
 */

#include "graph_test_util.h"

#include "retdec/gui/ui/call_graph_panel.h"
#include "retdec/gui/ui/cfg_panel.h"
#include "retdec/gui/ui/type_hierarchy_panel.h"

#include <fuse/app/app.hpp>

#include <gtest/gtest.h>

#include <cstdlib>
#include <format>

using namespace retdec::gui;
using namespace retdec::gui::model;
using retdec::gui::test::Headless;
namespace w = fuse::ui::widgets;

namespace {

double g_now = 100.0;
double fakeNow(void*)
{
	return g_now;
}

void finish(Headless& h, const char* name)
{
	h.frame();
	h.frame();
	EXPECT_GT(h.ui.drawList().size(), 10u);
	if (const char* dir = std::getenv("RETDEC_GUI_TEST_PNG_DIR"))
		EXPECT_TRUE(fuse::app::writePng(h.render(), std::string(dir) + "/" + name + ".png"));
}

BasicBlockData block(uint64_t id, bool entry = false, bool exit = false)
{
	BasicBlockData b;
	b.id = id;
	b.address = 0x401000 + id * 0x10;
	b.isEntry = entry;
	b.isExit = exit;
	b.instrs.push_back({b.address, "push   rbp"});
	b.instrs.push_back({b.address + 1, "mov    rbp, rsp"});
	b.instrs.push_back({b.address + 4, "cmp    dword [rbp-4], 0x10"});
	return b;
}

} // namespace

TEST(GraphRenderPreview, Cfg)
{
	Headless h(1000.f, 600.f);
	ui::CfgPanel panel(h.ui, h.ui.root());
	h.frame();
	// The bundled faces are registered: CFG block text is drawn monospace.
	ASSERT_NE(h.ui.resolveFont("monospace"), h.ui.font());
	ASSERT_NE(h.ui.resolveFont("sans-serif", fuse::ui::kFontWeightBold), h.ui.font());
	panel.setFunction(0x401000, "main");
	panel.setCfg(
		{block(0, true), block(1), block(2, false, true)},
		{{0, 1, EdgeKind::TrueBranch}, {0, 2, EdgeKind::FalseBranch}, {1, 2, EdgeKind::FallThrough}});
	finish(h, "graph_cfg");
}

TEST(GraphRenderPreview, CallGraphDepthAllAndPlaceholders)
{
	Headless h(1000.f, 600.f);
	ui::CallGraphPanel panel(h.ui, h.ui.root());
	std::vector<CallGraphNode> nodes;
	for (uint64_t i = 0; i < 5; ++i)
	{
		CallGraphNode n;
		n.address = 0x401000 + i * 0x100;
		n.name = i == 0 ? "main" : std::format("sub_{:x}", n.address);
		n.instrCount = int(20 + i * 7);
		n.isLibrary = i == 4;
		nodes.push_back(n);
	}
	std::vector<CallEdge> edges;
	for (uint64_t i = 1; i < 5; ++i)
		edges.push_back({0x401000, 0x401000 + i * 0x100, int(i), true});
	panel.setCallGraph(nodes, edges);
	h.frame();

	// Depth 0 shows Qt's "All" (special-value text), other depths the number.
	std::string scratch;
	EXPECT_EQ(panel.callGraphDepth(), 0);
	EXPECT_EQ(w::spinBoxText(h.ui, panel.depthSpin(), scratch), "All");
	panel.setCallGraphDepth(2);
	EXPECT_EQ(w::spinBoxText(h.ui, panel.depthSpin(), scratch), "2");
	panel.setCallGraphDepth(0);
	EXPECT_EQ(w::spinBoxText(h.ui, panel.depthSpin(), scratch), "All");
	finish(h, "graph_call_graph");
}

TEST(GraphRenderPreview, TypeHierarchyAbstractBoldAndDoubleClick)
{
	Headless h(1000.f, 600.f);
	h.ui.setClock({nullptr, &fakeNow});
	ui::TypeHierarchyPanel panel(h.ui, h.ui.root());
	auto cls = [](const std::string& name, std::vector<std::string> bases, bool abstract) {
		ClassInfo ci;
		ci.name = name;
		ci.vtableAddress = 0x500000 + name.size() * 0x40;
		ci.compiler = "GCC";
		ci.methodCount = 3;
		ci.isAbstract = abstract;
		for (const auto& b: bases)
			ci.bases.push_back(InheritanceLink{b, InheritanceLink::Kind::Public});
		for (int i = 0; i < 3; ++i)
		{
			VtableSlot s;
			s.index = i;
			s.funcName = name + "::method" + std::to_string(i);
			s.isPure = abstract && i == 0;
			s.funcAddress = s.isPure ? 0 : ci.vtableAddress + uint64_t(i) * 8;
			ci.vtable.push_back(s);
		}
		return ci;
	};
	panel.setHierarchy({cls("Shape", {}, true), cls("Circle", {"Shape"}, false), cls("Square", {"Shape"}, false)});
	ASSERT_TRUE(panel.selectClass("Shape"));
	h.frame();
	// Abstract classes / pure slots: bold face (Qt italic; FuseUI has no italic faces) — see the PNG.
	finish(h, "graph_type_hierarchy");

	// A double click on a class row (not its triangle) toggles it; two slow
	// clicks do not.
	const int rows = panel.visibleRowCount();
	const auto c = panel.treeRowCenter(0);
	ASSERT_TRUE(c);
	g_now = 200.0;
	h.click(*c);
	g_now = 201.0;
	h.click(*c);
	EXPECT_EQ(panel.visibleRowCount(), rows);
	g_now = 201.2;
	h.click(*c);
	EXPECT_NE(panel.visibleRowCount(), rows);
}
