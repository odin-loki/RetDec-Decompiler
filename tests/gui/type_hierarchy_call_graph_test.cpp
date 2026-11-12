/**
 * @file tests/gui/type_hierarchy_call_graph_test.cpp
 * @brief TypeHierarchyPanel + CallGraphPanel tests: port of the Qt type_hierarchy_call_graph_test plus headless
 *        FuseUI interaction tests.
 *
 * Type hierarchy: ClassHierarchyModel (tree, columns, headers, tooltip, colours, filter), VtableModel, panel
 *   (construction, setHierarchy, clear, filter, selection -> onClassSelected, vtable double click -> navigation,
 *   expand / collapse).
 * Call graph: CallGraphModel (empty, single node, chain, SCC collapse, filters, focus, DOT), node / edge geometry,
 *   panel (construction, load, clear, legacy names, click -> onFunctionActivated, toolbar filters, context menu focus,
 *   module combo, depth), screenshot.
 *
 * @copyright (c) 2026 Odin Loch trading as Imortek
 */

#include "graph_test_util.h"

#include "retdec/gui/model/callgraph_model.h"
#include "retdec/gui/model/typehier_model.h"
#include "retdec/gui/ui/call_graph_panel.h"
#include "retdec/gui/ui/type_hierarchy_panel.h"

#include <gtest/gtest.h>

#include <format>

using namespace retdec::gui;
using namespace retdec::gui::model;
using retdec::gui::test::Headless;
using retdec::gui::ui::CallGraphPanel;
using retdec::gui::ui::TypeHierarchyPanel;

namespace {

ClassInfo makeClass(const std::string& name, const std::vector<std::string>& bases = {}, int methods = 3,
	bool isAbstract = false)
{
	ClassInfo ci;
	ci.name = name;
	ci.vtableAddress = 0x1000 + uint64_t(name.size()) * 0x10;
	ci.compiler = "GCC";
	ci.methodCount = methods;
	ci.isAbstract = isAbstract;
	for (const auto& b: bases)
		ci.bases.push_back(InheritanceLink{b, InheritanceLink::Kind::Public});
	for (int i = 0; i < std::min(methods, 3); ++i)
	{
		VtableSlot slot;
		slot.index = i;
		slot.funcName = name + "::method" + std::to_string(i);
		slot.funcAddress = ci.vtableAddress + uint64_t(i * 8);
		slot.isPure = (i == 0 && isAbstract);
		ci.vtable.push_back(slot);
	}
	return ci;
}

CallGraphNode makeNode(uint64_t addr, const std::string& name, int instrCount = 50, bool isLibrary = false, int moduleId = -1)
{
	CallGraphNode n;
	n.address = addr;
	n.name = name;
	n.instrCount = instrCount;
	n.isLibrary = isLibrary;
	n.moduleId = moduleId;
	return n;
}

CallEdge makeEdge(uint64_t caller, uint64_t callee, int count = 1, bool direct = true)
{
	CallEdge e;
	e.callerAddress = caller;
	e.calleeAddress = callee;
	e.callCount = count;
	e.isDirect = direct;
	return e;
}

CallGraphModel loaded(std::vector<CallGraphNode> n, std::vector<CallEdge> e)
{
	CallGraphModel m;
	m.loadGraph(std::move(n), std::move(e));
	return m;
}

/// A small program: main -> parse / run / printf(lib); run -> eval <-> apply (SCC); modules 0 / 1.
std::pair<std::vector<CallGraphNode>, std::vector<CallEdge>> program()
{
	return {
		{
			makeNode(0x1000, "main", 120, false, 0),
			makeNode(0x1100, "parse_args", 300, false, 0),
			makeNode(0x1200, "run", 80, false, 1),
			makeNode(0x1300, "eval", 500, false, 1),
			makeNode(0x1400, "apply", 40, false, 1),
			makeNode(0x1500, "report", 60, false, 0),
			makeNode(0x9000, "printf", 10, true),
			makeNode(0x9100, "malloc", 10, true),
		},
		{
			makeEdge(0x1000, 0x1100, 1),
			makeEdge(0x1000, 0x1200, 1),
			makeEdge(0x1000, 0x9000, 7),
			makeEdge(0x1200, 0x1300, 3),
			makeEdge(0x1300, 0x1400, 12),
			makeEdge(0x1400, 0x1300, 9),
			makeEdge(0x1200, 0x1500, 1, false),
			makeEdge(0x1500, 0x9000, 4),
			makeEdge(0x1100, 0x9100, 2),
		},
	};
}

} // namespace

// ═════════════════════════════════════════════════════════════════════════════
// ClassHierarchyModel
// ═════════════════════════════════════════════════════════════════════════════

TEST(ClassHierarchyModelTest, EmptyModelHasNoRows)
{
	ClassHierarchyModel model;
	model.setClasses({});
	EXPECT_EQ(model.rowCount(), 0);
}

TEST(ClassHierarchyModelTest, SingleClassIsRoot)
{
	ClassHierarchyModel model;
	model.setClasses({makeClass("Foo")});
	EXPECT_EQ(model.rowCount(), 1);
}

TEST(ClassHierarchyModelTest, ChildClassUnderParent)
{
	ClassHierarchyModel model;
	model.setClasses({makeClass("Base"), makeClass("Derived", {"Base"})});
	EXPECT_EQ(model.rowCount(), 1);
	const int rootIdx = model.index(0);
	EXPECT_EQ(model.rowCount(rootIdx), 1);
	EXPECT_EQ(model.classAt(model.index(0, rootIdx))->name, "Derived");
	EXPECT_EQ(model.parent(model.index(0, rootIdx)), rootIdx);
}

TEST(ClassHierarchyModelTest, MultipleRoots)
{
	ClassHierarchyModel model;
	model.setClasses({makeClass("A"), makeClass("B"), makeClass("C")});
	EXPECT_EQ(model.rowCount(), 3);
}

TEST(ClassHierarchyModelTest, DeepHierarchy)
{
	ClassHierarchyModel model;
	model.setClasses({makeClass("A"), makeClass("B", {"A"}), makeClass("C", {"B"}), makeClass("D", {"C"})});
	EXPECT_EQ(model.rowCount(), 1);
	EXPECT_EQ(model.depth(model.findClass("D")), 3);
}

TEST(ClassHierarchyModelTest, ChildBeforeParentInInput)
{
	ClassHierarchyModel model;
	model.setClasses({makeClass("Derived", {"Base"}), makeClass("Base")});
	EXPECT_EQ(model.rowCount(), 1);
	EXPECT_EQ(model.classAt(model.index(0))->name, "Base");
}

TEST(ClassHierarchyModelTest, InheritanceCycleDoesNotLoseClasses)
{
	ClassHierarchyModel model;
	model.setClasses({makeClass("A", {"B"}), makeClass("B", {"A"})});
	ClassHierarchyModel::Row dummy;
	(void)dummy;
	EXPECT_EQ(model.visibleRows("", {}).size(), 2u);
}

TEST(ClassHierarchyModelTest, ColumnCount)
{
	ClassHierarchyModel model;
	EXPECT_EQ(model.columnCount(), 5);
}

TEST(ClassHierarchyModelTest, NameColumnData)
{
	ClassHierarchyModel model;
	model.setClasses({makeClass("MyClass")});
	EXPECT_NE(model.data(model.index(0), 0).find("MyClass"), std::string::npos);
}

TEST(ClassHierarchyModelTest, AbstractClassShowsFlag)
{
	ClassHierarchyModel model;
	model.setClasses({makeClass("Abstract", {}, 2, true)});
	EXPECT_NE(model.data(model.index(0), 0).find("abstract"), std::string::npos);
	EXPECT_TRUE(model.italic(model.index(0), 0));
	EXPECT_EQ(model.foreground(model.index(0), 0), catppuccin::mauve);
}

TEST(ClassHierarchyModelTest, VtableAddressColumn)
{
	ClassHierarchyModel model;
	auto cls = makeClass("Foo");
	cls.vtableAddress = 0xDEADBEEF;
	model.setClasses({cls});
	EXPECT_NE(model.data(model.index(0), 2).find("deadbeef"), std::string::npos);
}

TEST(ClassHierarchyModelTest, MethodCountColumn)
{
	ClassHierarchyModel model;
	model.setClasses({makeClass("Foo", {}, 7)});
	EXPECT_EQ(std::stoi(model.data(model.index(0), 3)), 7);
}

TEST(ClassHierarchyModelTest, InheritsColumnAndColours)
{
	ClassHierarchyModel model;
	auto d = makeClass("D", {"B"});
	d.bases.push_back(InheritanceLink{"V", InheritanceLink::Kind::Virtual});
	model.setClasses({makeClass("B"), d});
	const int dn = model.findClass("D");
	EXPECT_EQ(model.data(dn, 1), "public B, virtual V");
	EXPECT_EQ(model.foreground(dn, 1), catppuccin::green);
	EXPECT_EQ(model.data(model.findClass("B"), 1), "—");
	EXPECT_EQ(inheritColor(InheritanceLink::Kind::Private), catppuccin::red);
	EXPECT_STREQ(inheritLabel(InheritanceLink::Kind::Protected), "protected");
}

TEST(ClassHierarchyModelTest, HeaderLabels)
{
	EXPECT_FALSE(ClassHierarchyModel::headerData(0).empty());
	EXPECT_FALSE(ClassHierarchyModel::headerData(1).empty());
	EXPECT_FALSE(ClassHierarchyModel::headerData(2).empty());
	EXPECT_EQ(ClassHierarchyModel::headerData(4), "Compiler");
}

TEST(ClassHierarchyModelTest, InvalidIndexReturnsEmpty)
{
	ClassHierarchyModel model;
	EXPECT_TRUE(model.data(-1, 0).empty());
	EXPECT_TRUE(model.tooltip(-1).empty());
}

TEST(ClassHierarchyModelTest, ClassAtReturnsNullForInvalid)
{
	ClassHierarchyModel model;
	EXPECT_EQ(model.classAt(-1), nullptr);
}

TEST(ClassHierarchyModelTest, ClassAtReturnsCorrectClass)
{
	ClassHierarchyModel model;
	model.setClasses({makeClass("TestClass")});
	const ClassInfo* cls = model.classAt(model.index(0));
	ASSERT_NE(cls, nullptr);
	EXPECT_EQ(cls->name, "TestClass");
}

TEST(ClassHierarchyModelTest, TooltipListsModuleAndBases)
{
	ClassHierarchyModel model;
	auto c = makeClass("C", {"B"});
	c.moduleName = "widget.cpp";
	model.setClasses({makeClass("B"), c});
	const std::string tip = model.tooltip(model.findClass("C"));
	EXPECT_NE(tip.find("Module: widget.cpp"), std::string::npos);
	EXPECT_NE(tip.find("inherits public B"), std::string::npos);
	EXPECT_NE(tip.find("Methods: 3"), std::string::npos);
}

TEST(ClassHierarchyModelTest, RecursiveFilterKeepsAncestors)
{
	ClassHierarchyModel model;
	model.setClasses({makeClass("Shape"), makeClass("Circle", {"Shape"}), makeClass("Square", {"Shape"}),
		makeClass("Unrelated")});
	const auto rows = model.visibleRows("circ", {});
	ASSERT_EQ(rows.size(), 2u);
	EXPECT_EQ(model.classAt(rows[0].node)->name, "Shape");
	EXPECT_EQ(model.classAt(rows[1].node)->name, "Circle");
	EXPECT_EQ(rows[1].depth, 1);
	// collapsed parents hide their children
	const auto collapsed = model.visibleRows("", {model.findClass("Shape")});
	EXPECT_EQ(collapsed.size(), 2u);
	EXPECT_FALSE(collapsed[0].expanded);
	EXPECT_TRUE(collapsed[0].hasChildren);
}

// ═════════════════════════════════════════════════════════════════════════════
// VtableModel
// ═════════════════════════════════════════════════════════════════════════════

TEST(VtableModelTest, EmptyModelHasNoRows)
{
	VtableModel model;
	EXPECT_EQ(model.rowCount(), 0);
}

TEST(VtableModelTest, SetSlotsUpdatesRowCount)
{
	VtableModel model;
	std::vector<VtableSlot> slots;
	for (int i = 0; i < 5; ++i)
		slots.push_back(VtableSlot{i, std::format("func{}", i), 0x1000 + uint64_t(i * 8)});
	model.setSlots(slots);
	EXPECT_EQ(model.rowCount(), 5);
}

TEST(VtableModelTest, ColumnCount)
{
	VtableModel model;
	EXPECT_EQ(model.columnCount(), 4);
}

TEST(VtableModelTest, IndexColumnShowsNumber)
{
	VtableModel model;
	VtableSlot s;
	s.index = 3;
	s.funcName = "test";
	model.setSlots({s});
	EXPECT_EQ(std::stoi(model.data(0, 0)), 3);
}

TEST(VtableModelTest, FunctionNameColumn)
{
	VtableModel model;
	VtableSlot s;
	s.funcName = "MyClass::virtualMethod";
	model.setSlots({s});
	EXPECT_EQ(model.data(0, 1), "MyClass::virtualMethod");
}

TEST(VtableModelTest, PureFlagInFlagsColumn)
{
	VtableModel model;
	VtableSlot s;
	s.isPure = true;
	model.setSlots({s});
	EXPECT_NE(model.data(0, 3).find("pure"), std::string::npos);
	EXPECT_EQ(model.foreground(0, 1), catppuccin::mauve);
	EXPECT_TRUE(model.italic(0, 1));
}

TEST(VtableModelTest, FlagsAndAddressFormatting)
{
	VtableModel model;
	VtableSlot a{0, "f", 0x401000, false, true};
	VtableSlot b{1, "g", 0, false, false};
	model.setSlots({a, b});
	EXPECT_EQ(model.data(0, 3), "override");
	EXPECT_EQ(model.data(0, 2), "0x401000");
	EXPECT_EQ(model.data(1, 3), "—");
	EXPECT_EQ(model.data(1, 2), "—");
	EXPECT_EQ(model.tooltip(0), "0x401000: f");
	EXPECT_EQ(VtableModel::headerData(1), "Function");
}

TEST(VtableModelTest, SlotAtReturnsNull)
{
	VtableModel model;
	EXPECT_EQ(model.slotAt(0), nullptr);
}

TEST(VtableModelTest, SlotAtReturnsSlot)
{
	VtableModel model;
	VtableSlot s;
	s.funcName = "f";
	model.setSlots({s});
	ASSERT_NE(model.slotAt(0), nullptr);
	EXPECT_EQ(model.slotAt(0)->funcName, "f");
}

// ═════════════════════════════════════════════════════════════════════════════
// TypeHierarchyPanel
// ═════════════════════════════════════════════════════════════════════════════

TEST(TypeHierarchyPanelTest, ConstructsWithoutCrash)
{
	Headless h;
	EXPECT_NO_THROW({ TypeHierarchyPanel panel(h.ui, h.ui.root()); });
	h.frame();
}

TEST(TypeHierarchyPanelTest, SetHierarchyDoesNotCrash)
{
	Headless h;
	TypeHierarchyPanel panel(h.ui, h.ui.root());
	EXPECT_NO_THROW(panel.setHierarchy({makeClass("A"), makeClass("B", {"A"})}));
	h.frame();
	EXPECT_EQ(panel.visibleRowCount(), 2);
	EXPECT_EQ(panel.rowDepth(1), 1);
	EXPECT_FALSE(panel.emptyLabelVisible());
}

TEST(TypeHierarchyPanelTest, ClearDoesNotCrash)
{
	Headless h;
	TypeHierarchyPanel panel(h.ui, h.ui.root());
	panel.setHierarchy({makeClass("A")});
	EXPECT_NO_THROW(panel.clear());
	h.frame();
	EXPECT_EQ(panel.visibleRowCount(), 0);
	EXPECT_TRUE(panel.emptyLabelVisible());
	EXPECT_TRUE(h.findText("No RTTI classes recovered").valid());
}

TEST(TypeHierarchyPanelTest, LegacySetHierarchyCompat)
{
	Headless h;
	TypeHierarchyPanel panel(h.ui, h.ui.root());
	LegacyClassEntry e;
	e.name = "OldClass";
	e.vtableAddress = 0x1234;
	e.compiler = "MSVC";
	LegacyClassEntry d{"NewClass", 0x2000, "MSVC", {"OldClass"}};
	EXPECT_NO_THROW(panel.setHierarchy(std::vector<LegacyClassEntry>{e, d}));
	EXPECT_EQ(panel.visibleRowCount(), 2);
	EXPECT_EQ(panel.rowText(1, 1), "public OldClass");
	EXPECT_EQ(panel.rowText(0, 4), "MSVC");
}

TEST(TypeHierarchyPanelTest, ClassSelectedSignalOnClick)
{
	Headless h;
	TypeHierarchyPanel panel(h.ui, h.ui.root());
	std::vector<std::string> selected;
	panel.onClassSelected = [&](const std::string& n) { selected.push_back(n); };
	panel.setHierarchy({makeClass("Base"), makeClass("MyClass", {"Base"})});
	h.frame();
	const auto c = panel.treeRowCenter(1);
	ASSERT_TRUE(c);
	h.click(*c);
	ASSERT_EQ(selected.size(), 1u);
	EXPECT_EQ(selected[0], "MyClass");
	EXPECT_EQ(panel.selectedClass(), std::optional<std::string>("MyClass"));
	// the vtable viewer shows the class's slots
	EXPECT_EQ(panel.vtableRowCount(), 3);
	EXPECT_EQ(panel.vtableText(0, 1), "MyClass::method0");
	EXPECT_NE(panel.detailsText().find("Bases: public Base"), std::string::npos);
	// keyboard: Up selects the parent row
	h.key(fuse::ui::Key::Up);
	EXPECT_EQ(selected.back(), "Base");
	EXPECT_NE(panel.detailsText().find("Derived: MyClass"), std::string::npos);
}

TEST(TypeHierarchyPanelTest, VtableSlotNavigatedOnDoubleClick)
{
	Headless h;
	TypeHierarchyPanel panel(h.ui, h.ui.root());
	std::vector<uint64_t> slotNav, addrNav;
	panel.onVtableSlotActivated = [&](uint64_t a) { slotNav.push_back(a); };
	panel.onAddressNavigated = [&](uint64_t a) { addrNav.push_back(a); };
	auto cls = makeClass("Widget");
	cls.vtable[2].funcAddress = 0x1234;
	panel.setHierarchy({cls});
	ASSERT_TRUE(panel.selectClass("Widget"));
	h.frame();
	const auto c = panel.vtableRowCenter(2);
	ASSERT_TRUE(c);
	h.click(*c);
	EXPECT_TRUE(slotNav.empty()); // a single click only selects
	h.click(*c);
	ASSERT_EQ(slotNav.size(), 1u);
	EXPECT_EQ(slotNav[0], 0x1234u);
	ASSERT_EQ(addrNav.size(), 1u);
	EXPECT_EQ(addrNav[0], 0x1234u);
	// Enter on the selected slot activates too
	h.key(fuse::ui::Key::Enter);
	EXPECT_EQ(slotNav.size(), 2u);
	// slots without an address do not navigate
	auto empty = makeClass("E");
	empty.vtable[0].funcAddress = 0;
	panel.setHierarchy({empty});
	panel.selectClass("E");
	panel.activateVtableRow(0);
	EXPECT_EQ(slotNav.size(), 2u);
}

TEST(TypeHierarchyPanelTest, FilterBoxFiltersRecursively)
{
	Headless h;
	TypeHierarchyPanel panel(h.ui, h.ui.root());
	panel.setHierarchy({makeClass("Shape"), makeClass("Circle", {"Shape"}), makeClass("Square", {"Shape"}),
		makeClass("Logger")});
	h.frame();
	EXPECT_EQ(panel.visibleRowCount(), 4);
	h.ui.setFocus(panel.searchBox());
	h.type("squ");
	EXPECT_EQ(panel.visibleRowCount(), 2);
	EXPECT_EQ(panel.rowText(1, 0), "Square");
	panel.setFilter("");
	EXPECT_EQ(panel.visibleRowCount(), 4);
}

TEST(TypeHierarchyPanelTest, ToggleCollapsesSubtree)
{
	Headless h;
	TypeHierarchyPanel panel(h.ui, h.ui.root());
	panel.setHierarchy({makeClass("A"), makeClass("B", {"A"}), makeClass("C", {"B"})});
	h.frame();
	EXPECT_EQ(panel.visibleRowCount(), 3);
	const auto t = panel.treeToggleCenter(0);
	ASSERT_TRUE(t);
	h.click(*t);
	EXPECT_EQ(panel.visibleRowCount(), 1);
	h.click(*t);
	EXPECT_EQ(panel.visibleRowCount(), 3);
	panel.collapseAll();
	EXPECT_EQ(panel.visibleRowCount(), 1);
	ASSERT_TRUE(panel.selectClass("C")); // reveals
	EXPECT_EQ(panel.visibleRowCount(), 3);
	// Left on an expanded row collapses it
	panel.selectClass("B");
	h.frame();
	h.ui.setFocus(panel.treeTable());
	h.key(fuse::ui::Key::Left);
	EXPECT_EQ(panel.visibleRowCount(), 2);
	h.key(fuse::ui::Key::Right);
	EXPECT_EQ(panel.visibleRowCount(), 3);
}

TEST(TypeHierarchyPanelTest, RenderScreenshot)
{
	Headless h(900.f, 600.f);
	TypeHierarchyPanel panel(h.ui, h.ui.root());
	auto shape = makeClass("Shape", {}, 4, true);
	auto circle = makeClass("Circle", {"Shape"});
	circle.vtable[1].isOverride = true;
	auto square = makeClass("Square", {"Shape"});
	square.bases.push_back(InheritanceLink{"Printable", InheritanceLink::Kind::Virtual});
	auto prot = makeClass("Rounded", {"Square"});
	prot.bases[0].kind = InheritanceLink::Kind::Protected;
	panel.setHierarchy({shape, circle, square, prot, makeClass("Printable", {}, 1, true), makeClass("Logger")});
	panel.selectClass("Circle");
	const fuse::ui::Image img = h.render();
	EXPECT_TRUE(fuse::app::writePng(img, retdec::gui::test::shotPath("retdec_type_hierarchy.png").string()));
}

// ═════════════════════════════════════════════════════════════════════════════
// InheritanceLink
// ═════════════════════════════════════════════════════════════════════════════

TEST(InheritanceLinkTest, DefaultKindIsPublic)
{
	InheritanceLink link;
	EXPECT_EQ(link.kind, InheritanceLink::Kind::Public);
}

TEST(InheritanceLinkTest, AllKindsExist)
{
	InheritanceLink pub, prot, priv, virt;
	pub.kind = InheritanceLink::Kind::Public;
	prot.kind = InheritanceLink::Kind::Protected;
	priv.kind = InheritanceLink::Kind::Private;
	virt.kind = InheritanceLink::Kind::Virtual;
	EXPECT_NE(pub.kind, prot.kind);
	EXPECT_NE(pub.kind, priv.kind);
	EXPECT_NE(pub.kind, virt.kind);
}

// ═════════════════════════════════════════════════════════════════════════════
// CallGraphModel (Qt CallGraphScene tests)
// ═════════════════════════════════════════════════════════════════════════════

TEST(CallGraphSceneTest, EmptyGraphDoesNotCrash)
{
	CallGraphModel m;
	EXPECT_NO_THROW(m.loadGraph({}, {}));
	EXPECT_TRUE(m.scene().empty());
}

TEST(CallGraphSceneTest, SingleNodeGraph)
{
	auto m = loaded({makeNode(1, "main")}, {});
	EXPECT_EQ(m.visibleNodeCount(), 1);
}

TEST(CallGraphSceneTest, ThreeNodeLinearGraph)
{
	auto m = loaded({makeNode(1, "a"), makeNode(2, "b"), makeNode(3, "c")}, {makeEdge(1, 2), makeEdge(2, 3)});
	EXPECT_EQ(m.visibleNodeCount(), 3);
	EXPECT_EQ(m.scene().edges.size(), 2u);
}

TEST(CallGraphSceneTest, SccDetectionForCycle)
{
	auto m = loaded({makeNode(1, "a"), makeNode(2, "b"), makeNode(3, "c")}, {makeEdge(1, 2), makeEdge(2, 3), makeEdge(3, 1)});
	// SCC collapses to 1 super-node; visible node count = 0 (super-nodes are not function nodes)
	EXPECT_EQ(m.visibleNodeCount(), 0);
	EXPECT_EQ(m.visibleSccCount(), 1);
	ASSERT_EQ(m.sccGroups().size(), 1u);
	EXPECT_EQ(m.sccGroups().begin()->second.size(), 3u);
	EXPECT_EQ(m.sceneNodeFor(2), std::optional<uint64_t>(1));
	EXPECT_NE(m.scene().nodes[0].texts[0].text.find("SCC [3]"), std::string::npos);
	EXPECT_TRUE(m.scene().nodes[0].dashedBorder);
	// expanding SCCs shows the members
	m.setCollapseSccs(false);
	EXPECT_EQ(m.visibleNodeCount(), 3);
}

TEST(CallGraphSceneTest, ClearGraphResetsState)
{
	auto m = loaded({makeNode(1, "f")}, {});
	m.clearGraph();
	EXPECT_EQ(m.visibleNodeCount(), 0);
}

TEST(CallGraphSceneTest, ReloadReplacesGraph)
{
	CallGraphModel m;
	m.loadGraph({makeNode(1, "a"), makeNode(2, "b")}, {makeEdge(1, 2)});
	m.loadGraph({makeNode(10, "x")}, {});
	EXPECT_EQ(m.visibleNodeCount(), 1);
}

TEST(CallGraphSceneTest, HideLibraryFiltersLibNodes)
{
	auto m = loaded({makeNode(1, "myFunc", 50, false), makeNode(2, "printf", 10, true)}, {makeEdge(1, 2)});
	EXPECT_EQ(m.visibleNodeCount(), 2);
	m.setHideLibrary(true);
	EXPECT_EQ(m.visibleNodeCount(), 1);
	EXPECT_FALSE(m.scene().edges[0].visible); // edges to hidden nodes are hidden too
}

TEST(CallGraphSceneTest, NameFilterShowsMatchingNodes)
{
	auto m = loaded({makeNode(1, "alpha"), makeNode(2, "beta"), makeNode(3, "gamma")}, {});
	m.setNameFilter("*pha*");
	EXPECT_EQ(m.visibleNodeCount(), 1);
	m.setNameFilter("a*"); // whole-string glob, as Qt
	EXPECT_EQ(m.visibleNodeCount(), 1);
	m.setNameFilter("MM"); // plain text: case-insensitive substring
	EXPECT_EQ(m.visibleNodeCount(), 1);
}

TEST(CallGraphSceneTest, NameFilterEmpty_ShowsAll)
{
	auto m = loaded({makeNode(1, "f1"), makeNode(2, "f2"), makeNode(3, "f3")}, {});
	m.setNameFilter("");
	EXPECT_EQ(m.visibleNodeCount(), 3);
}

TEST(CallGraphSceneTest, FocusModeHidesUnrelatedNodes)
{
	auto m = loaded({makeNode(1, "a"), makeNode(2, "b"), makeNode(3, "c"), makeNode(4, "d")}, {makeEdge(1, 2), makeEdge(2, 3)});
	m.setFocusNode(2, 1);
	EXPECT_LT(m.visibleNodeCount(), 4);
	EXPECT_EQ(m.visibleNodeCount(), 3);
	EXPECT_FALSE(m.isNodeVisible(4));
	EXPECT_TRUE(m.isFocused());
}

TEST(CallGraphSceneTest, ClearFocusRestoresAll)
{
	auto m = loaded({makeNode(1, "a"), makeNode(2, "b"), makeNode(3, "c"), makeNode(4, "d")}, {makeEdge(1, 2), makeEdge(2, 3)});
	m.setFocusNode(1, 1);
	m.clearFocus();
	EXPECT_FALSE(m.isFocused());
	EXPECT_EQ(m.visibleNodeCount(), 4);
}

TEST(CallGraphSceneTest, DepthAndModuleAndMinCallFilters)
{
	auto [nodes, edges] = program();
	auto m = loaded(nodes, edges);
	EXPECT_EQ(m.depthOf(0x1000), std::optional<int>(0));
	EXPECT_EQ(m.depthOf(0x1300), std::optional<int>(2));
	m.setMaxDepth(1);
	EXPECT_FALSE(m.isNodeVisible(0x1300));
	EXPECT_TRUE(m.isNodeVisible(0x1200));
	m.setMaxDepth(-1);
	m.setModuleFilter(1);
	EXPECT_TRUE(m.isNodeVisible(0x1200));
	EXPECT_FALSE(m.isNodeVisible(0x1000));
	m.setModuleFilter(-1);
	EXPECT_EQ(m.moduleIds(), (std::vector<int>{0, 1}));
	// min call count hides light edges (not nodes)
	const int nodesBefore = m.visibleNodeCount();
	m.setMinCallCount(5);
	EXPECT_EQ(m.visibleNodeCount(), nodesBefore);
	int visibleEdges = 0;
	for (const auto& e: m.scene().edges)
		visibleEdges += e.visible;
	EXPECT_EQ(visibleEdges, 1); // main -> printf (7); eval <-> apply is inside the SCC
}

TEST(CallGraphSceneTest, NodeAndEdgeStyling)
{
	auto [nodes, edges] = program();
	auto m = loaded(nodes, edges);
	const SceneNode* mainNode = m.scene().node(0x1000);
	const SceneNode* printfNode = m.scene().node(0x9000);
	ASSERT_TRUE(mainNode && printfNode);
	EXPECT_EQ(mainNode->border, callGraphModuleColor(0));
	EXPECT_EQ(mainNode->stripeFill != 0, true);
	EXPECT_EQ(printfNode->fill, catppuccin::surface0); // library muted
	EXPECT_EQ(printfNode->stripeFill, 0u);
	EXPECT_NE(mainNode->tooltip.find("0x1000"), std::string::npos);
	EXPECT_NE(printfNode->tooltip.find("[library]"), std::string::npos);
	bool sawIndirect = false;
	for (const auto& e: m.scene().edges)
	{
		if (e.from == 0x1200 && e.to == 0x1500)
		{
			EXPECT_TRUE(e.dashed);
			sawIndirect = true;
		}
		if (e.from == 0x1000 && e.to == 0x9000) EXPECT_FLOAT_EQ(e.width, callGraphEdgeWidth(7));
	}
	EXPECT_TRUE(sawIndirect);
	EXPECT_GT(callGraphEdgeWidth(7), callGraphEdgeWidth(1));
	EXPECT_LE(callGraphEdgeWidth(1000000), 4.f);
	// module clusters, labelled
	ASSERT_EQ(m.scene().regions.size(), 2u);
	EXPECT_EQ(m.scene().regions[0].label, "Module 0");
}

TEST(CallGraphSceneTest, LayeredLayoutPutsCallersAboveCallees)
{
	auto [nodes, edges] = program();
	auto m = loaded(nodes, edges);
	m.setLayoutMode(CallGraphLayoutMode::Layered);
	const auto& sc = m.scene();
	EXPECT_LT(sc.node(0x1000)->rect.y, sc.node(0x1200)->rect.y);
	EXPECT_LT(sc.node(0x1200)->rect.y, sc.node(0x1500)->rect.y);
	EXPECT_LT(sc.node(0x1500)->rect.y, sc.node(0x9000)->rect.y);
}

TEST(CallGraphSceneTest, ForceLayoutSeparatesNodes)
{
	auto [nodes, edges] = program();
	auto m = loaded(nodes, edges);
	const auto& sc = m.scene();
	for (size_t i = 0; i < sc.nodes.size(); ++i)
		for (size_t j = i + 1; j < sc.nodes.size(); ++j)
		{
			const GPoint a = sc.nodes[i].rect.center(), b = sc.nodes[j].rect.center();
			EXPECT_GT(std::hypot(a.x - b.x, a.y - b.y), 20.f);
		}
	// deterministic
	auto m2 = loaded(nodes, edges);
	EXPECT_EQ(m2.scene().nodes[0].rect, sc.nodes[0].rect);
}

TEST(CallGraphSceneTest, DotExportContainsFunctions)
{
	auto m = loaded({makeNode(1, "main", 100), makeNode(2, "helper")}, {makeEdge(1, 2)});
	const std::string dot = m.exportDot();
	EXPECT_NE(dot.find("digraph"), std::string::npos);
	EXPECT_NE(dot.find("main"), std::string::npos);
	EXPECT_NE(dot.find("helper"), std::string::npos);
	EXPECT_NE(dot.find("->"), std::string::npos);
	EXPECT_NE(dot.find("\"1\" -> \"2\" [label=\"1\"]"), std::string::npos);
}

TEST(CallGraphSceneTest, DotExportEmptyGraph)
{
	CallGraphModel m;
	m.loadGraph({}, {});
	EXPECT_NE(m.exportDot().find("digraph"), std::string::npos);
}

TEST(CallGraphSceneTest, DotExportDashesLibrariesAndIndirectCalls)
{
	auto m = loaded({makeNode(1, "a<b>"), makeNode(2, "lib", 1, true)}, {makeEdge(1, 2, 3, false), makeEdge(1, 2, 5)});
	const std::string dot = m.exportDot();
	EXPECT_NE(dot.find("a&lt;b&gt;"), std::string::npos);
	EXPECT_NE(dot.find("\"2\" [label=\"lib\\n0x2\" style=dashed]"), std::string::npos);
	EXPECT_NE(dot.find("\"1\" -> \"2\" [label=\"3\" style=dashed]"), std::string::npos);
	EXPECT_EQ(dot.find("label=\"5\""), std::string::npos); // deduplicated
}

// ═════════════════════════════════════════════════════════════════════════════
// Node geometry (Qt CallGraphNodeItem / SccSuperNodeItem tests)
// ═════════════════════════════════════════════════════════════════════════════

TEST(CallGraphNodeItemTest, BoundingRectHasPositiveSize)
{
	auto m = loaded({makeNode(1, "test_func", 100)}, {});
	EXPECT_GT(m.scene().nodes[0].rect.w, 0);
	EXPECT_GT(m.scene().nodes[0].rect.h, 0);
}

TEST(CallGraphNodeItemTest, LargerFunctionIsWider)
{
	EXPECT_GE(callGraphNodeWidth(10000), callGraphNodeWidth(10));
	EXPECT_FLOAT_EQ(callGraphNodeWidth(0), callgraph::kMinNodeW);
	EXPECT_FLOAT_EQ(callGraphNodeWidth(1 << 30), callgraph::kMaxNodeW);
}

TEST(CallGraphNodeItemTest, DimmedStateChanges)
{
	// Focus mode hides (Qt: dims and hides) the nodes outside the neighbourhood, and restores them.
	auto m = loaded({makeNode(1, "f"), makeNode(2, "g")}, {});
	m.setFocusNode(1, 1);
	EXPECT_FALSE(m.scene().node(2)->visible);
	m.clearFocus();
	EXPECT_TRUE(m.scene().node(2)->visible);
}

TEST(SccSuperNodeItemTest, ConstructsWithMembers)
{
	auto m = loaded({makeNode(1, "a"), makeNode(2, "b"), makeNode(3, "c")}, {makeEdge(1, 2), makeEdge(2, 3), makeEdge(3, 1)});
	EXPECT_TRUE(m.isSccNode(1));
}

TEST(SccSuperNodeItemTest, BoundingRectPositive)
{
	auto m = loaded({makeNode(1, "f1"), makeNode(2, "f2")}, {makeEdge(1, 2), makeEdge(2, 1)});
	ASSERT_EQ(m.scene().nodes.size(), 1u);
	EXPECT_FLOAT_EQ(m.scene().nodes[0].rect.w, callgraph::kSccW);
	EXPECT_FLOAT_EQ(m.scene().nodes[0].rect.h, callgraph::kSccH);
	EXPECT_NE(m.scene().nodes[0].tooltip.find("2 mutually recursive"), std::string::npos);
}

TEST(SccSuperNodeItemTest, MembersReturned)
{
	auto m = loaded({makeNode(10, "a"), makeNode(20, "b"), makeNode(30, "c")},
		{makeEdge(10, 20), makeEdge(20, 30), makeEdge(30, 10)});
	EXPECT_EQ(m.sccGroups().at(10).size(), 3u);
}

// ═════════════════════════════════════════════════════════════════════════════
// CallGraphPanel
// ═════════════════════════════════════════════════════════════════════════════

TEST(CallGraphPanelTest, ConstructsWithoutCrash)
{
	Headless h;
	EXPECT_NO_THROW({ CallGraphPanel panel(h.ui, h.ui.root()); });
	h.frame();
}

TEST(CallGraphPanelTest, LoadGraphDoesNotCrash)
{
	Headless h;
	CallGraphPanel panel(h.ui, h.ui.root());
	EXPECT_NO_THROW(panel.setCallGraph({makeNode(1, "main"), makeNode(2, "sub")}, {makeEdge(1, 2)}));
	h.frame();
	EXPECT_EQ(panel.infoLabel(), "2 nodes");
	EXPECT_TRUE(h.findText("2 nodes").valid());
}

TEST(CallGraphPanelTest, ClearDoesNotCrash)
{
	Headless h;
	CallGraphPanel panel(h.ui, h.ui.root());
	panel.setCallGraph({makeNode(1, "f")}, {});
	panel.setHideLibrary(true);
	panel.setCallGraphDepth(3);
	EXPECT_NO_THROW(panel.clear());
	h.frame();
	EXPECT_TRUE(panel.graph().nodes().empty());
	EXPECT_EQ(panel.callGraphDepth(), 0);
	EXPECT_FALSE(h.ui.checked(panel.hideLibraryCheck()));
	EXPECT_EQ(panel.infoLabel(), "");
}

TEST(CallGraphPanelTest, LegacySetCallGraphCompat)
{
	Headless h;
	CallGraphPanel panel(h.ui, h.ui.root());
	EXPECT_NO_THROW(panel.setCallGraph(std::vector<std::string>{"funcA", "funcB"}, {makeEdge(0, 1)}));
	EXPECT_EQ(panel.graph().node(1)->name, "funcB");
}

TEST(CallGraphPanelTest, FunctionNavigationOnClick)
{
	Headless h;
	CallGraphPanel panel(h.ui, h.ui.root());
	std::vector<std::pair<uint64_t, std::string>> got;
	panel.onFunctionActivated = [&](uint64_t a, const std::string& n) { got.emplace_back(a, n); };
	auto [nodes, edges] = program();
	panel.setCallGraph(nodes, edges);
	h.frame();
	const auto c = panel.view().nodeCenter(0x1200);
	ASSERT_TRUE(c);
	h.click(*c);
	ASSERT_EQ(got.size(), 1u);
	EXPECT_EQ(got[0].first, 0x1200u);
	EXPECT_EQ(got[0].second, "run");
	// clicking the SCC super-node does not navigate; double clicking it expands the SCCs
	const auto scc = panel.view().nodeCenter(0x1300);
	ASSERT_TRUE(scc);
	h.click(*scc);
	h.click(*scc);
	h.frame(); // posted
	EXPECT_EQ(got.size(), 1u);
	EXPECT_FALSE(panel.graph().collapseSccs());
	EXPECT_TRUE(panel.view().nodeCenter(0x1400).has_value());
}

TEST(CallGraphPanelTest, OnFunctionSelectedDoesNotCrash)
{
	Headless h;
	CallGraphPanel panel(h.ui, h.ui.root());
	EXPECT_NO_THROW(panel.setFunction(0x1234, "main"));
	EXPECT_EQ(panel.focusLabel(), "main");
	auto [nodes, edges] = program();
	panel.setCallGraph(nodes, edges);
	h.frame();
	panel.setFunction(0x1100, "parse_args");
	EXPECT_EQ(panel.view().selectedNode(), std::optional<uint64_t>(0x1100));
}

TEST(CallGraphPanelTest, ToolbarFiltersDriveTheModel)
{
	Headless h(1300.f, 800.f);
	CallGraphPanel panel(h.ui, h.ui.root());
	auto [nodes, edges] = program();
	panel.setCallGraph(nodes, edges);
	h.frame();
	EXPECT_EQ(panel.visibleNodeCount(), 6);
	// Hide lib checkbox
	h.clickNode(panel.hideLibraryCheck());
	EXPECT_TRUE(panel.graph().hideLibrary());
	EXPECT_EQ(panel.visibleNodeCount(), 4);
	EXPECT_EQ(panel.infoLabel(), "4 visible");
	EXPECT_FALSE(panel.view().scene().node(0x9000)->visible);
	h.clickNode(panel.hideLibraryCheck());
	EXPECT_EQ(panel.visibleNodeCount(), 6);
	// Name filter typed into the box
	h.ui.setFocus(panel.nameFilterBox());
	h.type("par");
	EXPECT_EQ(panel.visibleNodeCount(), 1);
	panel.setNameFilter("");
	EXPECT_EQ(panel.visibleNodeCount(), 6);
	// Depth spin box (Up arrow steps it)
	h.ui.setFocus(panel.depthSpin());
	h.key(fuse::ui::Key::Up);
	EXPECT_EQ(panel.callGraphDepth(), 1);
	EXPECT_EQ(panel.graph().maxDepth(), 1);
	panel.setCallGraphDepth(0);
	EXPECT_EQ(panel.graph().maxDepth(), -1);
	// Module combo
	EXPECT_EQ(panel.moduleItems(), (std::vector<std::string>{"All modules", "Module 0", "Module 1"}));
	panel.setModuleFilter(1);
	EXPECT_EQ(h.ui.value(panel.moduleCombo()), 2.f);
	EXPECT_EQ(panel.visibleNodeCount(), 1); // run (eval / apply are the SCC)
	// reload keeps the selected module
	panel.setCallGraph(nodes, edges);
	EXPECT_EQ(panel.graph().moduleFilter(), 1);
	panel.setModuleFilter(-1);
	// Layout combo
	panel.setLayoutMode(CallGraphLayoutMode::Layered);
	EXPECT_EQ(h.ui.value(panel.layoutCombo()), 1.f);
	EXPECT_EQ(panel.graph().layoutMode(), CallGraphLayoutMode::Layered);
}

TEST(CallGraphPanelTest, ContextMenuFocusAndClearFocus)
{
	Headless h(1300.f, 800.f);
	CallGraphPanel panel(h.ui, h.ui.root());
	auto [nodes, edges] = program();
	panel.setCallGraph(nodes, edges);
	h.frame();
	EXPECT_FALSE(panel.clearFocusEnabled());
	const auto c = panel.view().nodeCenter(0x1500);
	ASSERT_TRUE(c);
	h.click(*c, fuse::ui::MouseButton::Right);
	const auto item = h.findText("Focus (1 hop)", fuse::ui::widgets::overlayLayer(h.ui));
	ASSERT_TRUE(item.valid());
	h.clickNode(item);
	h.frame();
	EXPECT_TRUE(panel.graph().isFocused());
	EXPECT_EQ(panel.graph().focusHops(), 1);
	EXPECT_EQ(panel.focusLabel(), "Focus: 0x1500 (1 hops)");
	EXPECT_TRUE(panel.clearFocusEnabled());
	EXPECT_EQ(panel.visibleNodeCount(), 3); // report, run, printf
	h.clickNode(panel.clearFocusButton());
	EXPECT_FALSE(panel.graph().isFocused());
	EXPECT_EQ(panel.visibleNodeCount(), 6);
	EXPECT_FALSE(panel.clearFocusEnabled());
	// Navigate to Function from the menu
	uint64_t nav = 0;
	panel.onFunctionActivated = [&](uint64_t a, const std::string&) { nav = a; };
	panel.runNodeMenu(0x1100, 4);
	EXPECT_EQ(nav, 0x1100u);
	EXPECT_EQ(panel.nodeMenuItems(0x1100)[4], "Navigate to Function");
}

TEST(CallGraphPanelTest, ExportsAndSearch)
{
	Headless h;
	CallGraphPanel panel(h.ui, h.ui.root());
	auto [nodes, edges] = program();
	panel.setCallGraph(nodes, edges);
	h.frame();
	EXPECT_NE(panel.exportDot().find("parse_args"), std::string::npos);
	const std::string svg = panel.exportSvg();
	EXPECT_NE(svg.find("<svg"), std::string::npos);
	EXPECT_NE(svg.find("Module 1"), std::string::npos);
	EXPECT_NE(svg.find("SCC [2]"), std::string::npos);
	ui::GraphExportFormat req = ui::GraphExportFormat::Svg;
	panel.onExportRequested = [&](ui::GraphExportFormat f) { req = f; };
	h.clickNode(panel.button(ui::GraphExportFormat::Dot));
	EXPECT_EQ(req, ui::GraphExportFormat::Dot);
	EXPECT_EQ(panel.search("apply"), 1); // finds the SCC node holding it
	EXPECT_EQ(panel.view().selectedNode(), std::optional<uint64_t>(0x1300));
	h.ui.setFocus(panel.findBox());
	h.type("parse");
	ASSERT_EQ(panel.view().highlighted().size(), 1u);
	EXPECT_TRUE(panel.view().highlighted().count(0x1100));
	h.key(fuse::ui::Key::Escape); // the find box keeps focus; Escape in the canvas clears
	h.ui.setFocus(panel.view().node());
	h.key(fuse::ui::Key::Escape);
	EXPECT_TRUE(panel.view().highlighted().empty());
}

TEST(CallGraphPanelTest, LargeGraphLaysOutQuickly)
{
	// A few thousand nodes: grid-accelerated force layout + culling paint.
	std::vector<CallGraphNode> nodes;
	std::vector<CallEdge> edges;
	for (uint64_t i = 0; i < 3000; ++i)
	{
		nodes.push_back(makeNode(0x10000 + i, std::format("fn_{}", i), int(i % 200), i % 17 == 0, int(i % 6)));
		if (i) edges.push_back(makeEdge(0x10000 + (i - 1) / 3, 0x10000 + i, int(i % 5) + 1));
	}
	Headless h;
	CallGraphPanel panel(h.ui, h.ui.root());
	const auto t0 = std::chrono::steady_clock::now();
	panel.setCallGraph(nodes, edges);
	h.frame();
	panel.view().resetZoom();
	h.frame();
	const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
	EXPECT_EQ(panel.visibleNodeCount(), 3000);
	EXPECT_LT(secs, 30.0); // Debug build; Release is a small fraction of this
	EXPECT_LT(h.ui.drawList().size(), 200000u); // culled
}

TEST(CallGraphPanelTest, RenderScreenshot)
{
	Headless h(1300.f, 800.f);
	CallGraphPanel panel(h.ui, h.ui.root());
	auto [nodes, edges] = program();
	panel.setCallGraph(nodes, edges);
	panel.setFunction(0x1200, "run");
	const fuse::ui::Image img = h.render();
	EXPECT_TRUE(fuse::app::writePng(img, retdec::gui::test::shotPath("retdec_call_graph.png").string()));
	panel.setLayoutMode(CallGraphLayoutMode::Layered);
	const fuse::ui::Image img2 = h.render();
	EXPECT_TRUE(fuse::app::writePng(img2, retdec::gui::test::shotPath("retdec_call_graph_layered.png").string()));
}

// ═════════════════════════════════════════════════════════════════════════════
// Data model defaults
// ═════════════════════════════════════════════════════════════════════════════

TEST(CallGraphDataModelTest, NodeDefaults)
{
	CallGraphNode n;
	EXPECT_EQ(n.address, 0u);
	EXPECT_FALSE(n.isLibrary);
	EXPECT_EQ(n.moduleId, -1);
	EXPECT_EQ(n.instrCount, 0);
}

TEST(CallGraphDataModelTest, EdgeDefaults)
{
	CallEdge e;
	EXPECT_EQ(e.callerAddress, 0u);
	EXPECT_EQ(e.calleeAddress, 0u);
	EXPECT_EQ(e.callCount, 1);
	EXPECT_TRUE(e.isDirect);
}

TEST(ClassInfoDataModelTest, Defaults)
{
	ClassInfo ci;
	EXPECT_EQ(ci.vtableAddress, 0u);
	EXPECT_FALSE(ci.isAbstract);
	EXPECT_EQ(ci.methodCount, 0);
	EXPECT_TRUE(ci.name.empty());
	EXPECT_TRUE(ci.bases.empty());
	EXPECT_TRUE(ci.vtable.empty());
}

TEST(VtableSlotDataModelTest, Defaults)
{
	VtableSlot s;
	EXPECT_EQ(s.index, 0);
	EXPECT_EQ(s.funcAddress, 0u);
	EXPECT_FALSE(s.isPure);
	EXPECT_FALSE(s.isOverride);
}
