/**
 * @file tests/gui/type_hierarchy_call_graph_test.cpp
 * @brief Unit tests for TypeHierarchyPanel + CallGraphPanel (Task 52).
 *
 * TypeHierarchyPanel tests:
 *   - ClassHierarchyModel: empty, single class, parent-child, deep hierarchy
 *   - ClassHierarchyModel: column data, headers, tooltip, foreground colours
 *   - VtableModel: empty, slots, data, headers
 *   - TypeHierarchyPanel: construction, setHierarchy, clear, filter, signals
 *
 * CallGraphPanel tests:
 *   - CallGraphScene: empty, single node, three nodes, SCC detection
 *   - CallGraphScene: filter by name, filter hide-library, focus mode
 *   - CallGraphScene: DOT export
 *   - CallGraphPanel: construction, loadGraph, clear, navigation signal
 *   - CallGraphNodeItem: bounding rect, dimmed state
 *   - SccSuperNodeItem: construction with members
 *   - CallGraphEdgeItem: construction
 */

#include <gtest/gtest.h>
#include <QApplication>
#include <QSignalSpy>

#include "retdec/gui/panels/type_hierarchy_panel.h"
#include "retdec/gui/panels/call_graph_panel.h"

#include "qt_test_env.h"

using namespace retdec::gui::panels;

// ─── Qt (single QApplication lives in qt_test_main.cpp) ──────────────────────
namespace {

void qtEnv3() {
    Q_ASSERT(QApplication::instance() != nullptr);
}

} // anonymous namespace

// ─── Helpers ─────────────────────────────────────────────────────────────────

static ClassInfo makeClass(const QString& name,
                            const QStringList& bases = {},
                            int methods = 3,
                            bool isAbstract = false)
{
    ClassInfo ci;
    ci.name        = name;
    ci.vtableAddress = 0x1000 + static_cast<uint64_t>(name.length()) * 0x10;
    ci.compiler    = "GCC";
    ci.methodCount = methods;
    ci.isAbstract  = isAbstract;
    for (const auto& b : bases) {
        InheritanceLink link;
        link.base = b;
        link.kind = InheritanceLink::Kind::Public;
        ci.bases.append(link);
    }
    // Add some vtable slots
    for (int i = 0; i < std::min(methods, 3); ++i) {
        VtableSlot slot;
        slot.index      = i;
        slot.funcName   = name + "::method" + QString::number(i);
        slot.funcAddress = ci.vtableAddress + static_cast<uint64_t>(i * 8);
        slot.isPure     = (i == 0 && isAbstract);
        ci.vtable.append(slot);
    }
    return ci;
}

static CallGraphNode makeNode(uint64_t addr, const QString& name,
                               int instrCount = 50,
                               bool isLibrary = false,
                               int moduleId = -1)
{
    CallGraphNode n;
    n.address    = addr;
    n.name       = name;
    n.instrCount = instrCount;
    n.isLibrary  = isLibrary;
    n.moduleId   = moduleId;
    return n;
}

static CallEdge makeEdge(uint64_t caller, uint64_t callee,
                          int count = 1, bool direct = true)
{
    CallEdge e;
    e.callerAddress = caller;
    e.calleeAddress = callee;
    e.callCount     = count;
    e.isDirect      = direct;
    return e;
}

// ═════════════════════════════════════════════════════════════════════════════
// ClassHierarchyModel tests
// ═════════════════════════════════════════════════════════════════════════════

TEST(ClassHierarchyModelTest, EmptyModelHasNoRows) {
    qtEnv3();
    ClassHierarchyModel model;
    model.setClasses({});
    EXPECT_EQ(model.rowCount(), 0);
}

TEST(ClassHierarchyModelTest, SingleClassIsRoot) {
    qtEnv3();
    ClassHierarchyModel model;
    model.setClasses({makeClass("Foo")});
    EXPECT_EQ(model.rowCount(), 1);
}

TEST(ClassHierarchyModelTest, ChildClassUnderParent) {
    qtEnv3();
    ClassHierarchyModel model;
    auto parent = makeClass("Base");
    auto child  = makeClass("Derived", {"Base"});
    model.setClasses({parent, child});
    // Root count should be 1 (just Base)
    EXPECT_EQ(model.rowCount(), 1);
    QModelIndex rootIdx = model.index(0, 0);
    EXPECT_EQ(model.rowCount(rootIdx), 1); // Derived is child of Base
}

TEST(ClassHierarchyModelTest, MultipleRoots) {
    qtEnv3();
    ClassHierarchyModel model;
    model.setClasses({makeClass("A"), makeClass("B"), makeClass("C")});
    EXPECT_EQ(model.rowCount(), 3);
}

TEST(ClassHierarchyModelTest, DeepHierarchy) {
    qtEnv3();
    ClassHierarchyModel model;
    auto a = makeClass("A");
    auto b = makeClass("B", {"A"});
    auto c = makeClass("C", {"B"});
    auto d = makeClass("D", {"C"});
    model.setClasses({a, b, c, d});
    EXPECT_EQ(model.rowCount(), 1); // only A is root
}

TEST(ClassHierarchyModelTest, ColumnCount) {
    qtEnv3();
    ClassHierarchyModel model;
    EXPECT_EQ(model.columnCount(), 5);
}

TEST(ClassHierarchyModelTest, NameColumnData) {
    qtEnv3();
    ClassHierarchyModel model;
    model.setClasses({makeClass("MyClass")});
    QModelIndex idx = model.index(0, 0);
    EXPECT_TRUE(model.data(idx, Qt::DisplayRole).toString().contains("MyClass"));
}

TEST(ClassHierarchyModelTest, AbstractClassShowsFlag) {
    qtEnv3();
    ClassHierarchyModel model;
    model.setClasses({makeClass("Abstract", {}, 2, true)});
    QModelIndex idx = model.index(0, 0);
    QString name = model.data(idx, Qt::DisplayRole).toString();
    EXPECT_TRUE(name.contains("abstract"));
}

TEST(ClassHierarchyModelTest, VtableAddressColumn) {
    qtEnv3();
    ClassHierarchyModel model;
    auto cls = makeClass("Foo");
    cls.vtableAddress = 0xDEADBEEF;
    model.setClasses({cls});
    QModelIndex idx = model.index(0, 2);
    EXPECT_TRUE(model.data(idx).toString().contains("deadbeef", Qt::CaseInsensitive));
}

TEST(ClassHierarchyModelTest, MethodCountColumn) {
    qtEnv3();
    ClassHierarchyModel model;
    auto cls = makeClass("Foo", {}, 7);
    model.setClasses({cls});
    QModelIndex idx = model.index(0, 3);
    EXPECT_EQ(model.data(idx).toInt(), 7);
}

TEST(ClassHierarchyModelTest, HeaderLabels) {
    qtEnv3();
    ClassHierarchyModel model;
    EXPECT_FALSE(model.headerData(0, Qt::Horizontal, Qt::DisplayRole).toString().isEmpty());
    EXPECT_FALSE(model.headerData(1, Qt::Horizontal, Qt::DisplayRole).toString().isEmpty());
    EXPECT_FALSE(model.headerData(2, Qt::Horizontal, Qt::DisplayRole).toString().isEmpty());
}

TEST(ClassHierarchyModelTest, InvalidIndexReturnsEmpty) {
    qtEnv3();
    ClassHierarchyModel model;
    EXPECT_FALSE(model.data(QModelIndex()).isValid());
}

TEST(ClassHierarchyModelTest, ClassAtReturnsNullForInvalid) {
    qtEnv3();
    ClassHierarchyModel model;
    EXPECT_EQ(model.classAt(QModelIndex()), nullptr);
}

TEST(ClassHierarchyModelTest, ClassAtReturnsCorrectClass) {
    qtEnv3();
    ClassHierarchyModel model;
    model.setClasses({makeClass("TestClass")});
    QModelIndex idx = model.index(0, 0);
    const ClassInfo* cls = model.classAt(idx);
    ASSERT_NE(cls, nullptr);
    EXPECT_EQ(cls->name, "TestClass");
}

// ═════════════════════════════════════════════════════════════════════════════
// VtableModel tests
// ═════════════════════════════════════════════════════════════════════════════

TEST(VtableModelTest, EmptyModelHasNoRows) {
    qtEnv3();
    VtableModel model;
    EXPECT_EQ(model.rowCount(), 0);
}

TEST(VtableModelTest, SetSlotsUpdatesRowCount) {
    qtEnv3();
    VtableModel model;
    QList<VtableSlot> vtableSlots;
    for (int i = 0; i < 5; ++i) {
        VtableSlot s;
        s.index = i;
        s.funcName = QString("func%1").arg(i);
        s.funcAddress = 0x1000 + static_cast<uint64_t>(i * 8);
        vtableSlots.append(s);
    }
    model.setSlots(vtableSlots);
    EXPECT_EQ(model.rowCount(), 5);
}

TEST(VtableModelTest, ColumnCount) {
    qtEnv3();
    VtableModel model;
    EXPECT_EQ(model.columnCount(), 4);
}

TEST(VtableModelTest, IndexColumnShowsNumber) {
    qtEnv3();
    VtableModel model;
    VtableSlot s;
    s.index = 3;
    s.funcName = "test";
    model.setSlots({s});
    EXPECT_EQ(model.data(model.index(0, 0)).toInt(), 3);
}

TEST(VtableModelTest, FunctionNameColumn) {
    qtEnv3();
    VtableModel model;
    VtableSlot s;
    s.funcName = "MyClass::virtualMethod";
    model.setSlots({s});
    EXPECT_EQ(model.data(model.index(0, 1)).toString(), "MyClass::virtualMethod");
}

TEST(VtableModelTest, PureFlagInFlagsColumn) {
    qtEnv3();
    VtableModel model;
    VtableSlot s;
    s.isPure = true;
    model.setSlots({s});
    EXPECT_TRUE(model.data(model.index(0, 3)).toString().contains("pure"));
}

TEST(VtableModelTest, SlotAtReturnsNull) {
    qtEnv3();
    VtableModel model;
    EXPECT_EQ(model.slotAt(0), nullptr);
}

TEST(VtableModelTest, SlotAtReturnsSlot) {
    qtEnv3();
    VtableModel model;
    VtableSlot s;
    s.funcName = "f";
    model.setSlots({s});
    ASSERT_NE(model.slotAt(0), nullptr);
    EXPECT_EQ(model.slotAt(0)->funcName, "f");
}

// ═════════════════════════════════════════════════════════════════════════════
// TypeHierarchyPanel tests
// ═════════════════════════════════════════════════════════════════════════════

TEST(TypeHierarchyPanelTest, ConstructsWithoutCrash) {
    qtEnv3();
    EXPECT_NO_THROW({ TypeHierarchyPanel panel; });
}

TEST(TypeHierarchyPanelTest, SetHierarchyDoesNotCrash) {
    qtEnv3();
    TypeHierarchyPanel panel;
    EXPECT_NO_THROW(panel.setHierarchy({makeClass("A"), makeClass("B", {"A"})}));
}

TEST(TypeHierarchyPanelTest, ClearDoesNotCrash) {
    qtEnv3();
    TypeHierarchyPanel panel;
    panel.setHierarchy({makeClass("A")});
    EXPECT_NO_THROW(panel.clear());
}

TEST(TypeHierarchyPanelTest, LegacySetHierarchyCompat) {
    qtEnv3();
    TypeHierarchyPanel panel;
    TypeHierarchyPanel::ClassEntry e;
    e.name = "OldClass";
    e.vtableAddress = 0x1234;
    e.compiler = "MSVC";
    EXPECT_NO_THROW(panel.setHierarchy({e}));
}

TEST(TypeHierarchyPanelTest, ClassSelectedSignalExists) {
    qtEnv3();
    TypeHierarchyPanel panel;
    QSignalSpy spy(&panel, &TypeHierarchyPanel::classSelected);
    emit panel.classSelected("MyClass");
    EXPECT_EQ(spy.count(), 1);
}

TEST(TypeHierarchyPanelTest, VtableSlotNavigatedSignalExists) {
    qtEnv3();
    TypeHierarchyPanel panel;
    QSignalSpy spy(&panel, &TypeHierarchyPanel::vtableSlotNavigated);
    emit panel.vtableSlotNavigated(0x1234);
    EXPECT_EQ(spy.count(), 1);
    EXPECT_EQ(spy.takeFirst().at(0).value<uint64_t>(), 0x1234u);
}

// ═════════════════════════════════════════════════════════════════════════════
// InheritanceLink tests
// ═════════════════════════════════════════════════════════════════════════════

TEST(InheritanceLinkTest, DefaultKindIsPublic) {
    InheritanceLink link;
    EXPECT_EQ(link.kind, InheritanceLink::Kind::Public);
}

TEST(InheritanceLinkTest, AllKindsExist) {
    InheritanceLink pub, prot, priv, virt;
    pub.kind  = InheritanceLink::Kind::Public;
    prot.kind = InheritanceLink::Kind::Protected;
    priv.kind = InheritanceLink::Kind::Private;
    virt.kind = InheritanceLink::Kind::Virtual;
    EXPECT_NE(pub.kind, prot.kind);
    EXPECT_NE(pub.kind, priv.kind);
    EXPECT_NE(pub.kind, virt.kind);
}

// ═════════════════════════════════════════════════════════════════════════════
// CallGraphScene tests
// ═════════════════════════════════════════════════════════════════════════════

TEST(CallGraphSceneTest, EmptyGraphDoesNotCrash) {
    qtEnv3();
    CallGraphScene scene;
    EXPECT_NO_THROW(scene.loadGraph({}, {}));
}

TEST(CallGraphSceneTest, SingleNodeGraph) {
    qtEnv3();
    CallGraphScene scene;
    scene.loadGraph({makeNode(1, "main")}, {});
    EXPECT_EQ(scene.visibleNodeCount(), 1);
}

TEST(CallGraphSceneTest, ThreeNodeLinearGraph) {
    qtEnv3();
    CallGraphScene scene;
    scene.loadGraph(
        {makeNode(1, "a"), makeNode(2, "b"), makeNode(3, "c")},
        {makeEdge(1, 2), makeEdge(2, 3)});
    EXPECT_EQ(scene.visibleNodeCount(), 3);
}

TEST(CallGraphSceneTest, SccDetectionForCycle) {
    qtEnv3();
    CallGraphScene scene;
    // a→b→c→a is a 3-node SCC — should be collapsed to SCC super-node
    scene.loadGraph(
        {makeNode(1, "a"), makeNode(2, "b"), makeNode(3, "c")},
        {makeEdge(1, 2), makeEdge(2, 3), makeEdge(3, 1)});
    // SCC collapses to 1 super-node, visible node count = 0 (SCC items not in nodeItems)
    EXPECT_EQ(scene.visibleNodeCount(), 0);
}

TEST(CallGraphSceneTest, ClearGraphResetsState) {
    qtEnv3();
    CallGraphScene scene;
    scene.loadGraph({makeNode(1, "f")}, {});
    scene.clearGraph();
    EXPECT_EQ(scene.visibleNodeCount(), 0);
}

TEST(CallGraphSceneTest, ReloadReplacesGraph) {
    qtEnv3();
    CallGraphScene scene;
    scene.loadGraph({makeNode(1, "a"), makeNode(2, "b")}, {makeEdge(1, 2)});
    scene.loadGraph({makeNode(10, "x")}, {});
    EXPECT_EQ(scene.visibleNodeCount(), 1);
}

TEST(CallGraphSceneTest, HideLibraryFiltersLibNodes) {
    qtEnv3();
    CallGraphScene scene;
    scene.loadGraph(
        {makeNode(1, "myFunc", 50, false), makeNode(2, "printf", 10, true)},
        {makeEdge(1, 2)});
    EXPECT_EQ(scene.visibleNodeCount(), 2);
    scene.setHideLibrary(true);
    EXPECT_EQ(scene.visibleNodeCount(), 1);
}

TEST(CallGraphSceneTest, NameFilterShowsMatchingNodes) {
    qtEnv3();
    CallGraphScene scene;
    scene.loadGraph(
        {makeNode(1, "alpha"), makeNode(2, "beta"), makeNode(3, "gamma")}, {});
    scene.setNameFilter("*pha*");
    EXPECT_EQ(scene.visibleNodeCount(), 1);
}

TEST(CallGraphSceneTest, NameFilterEmpty_ShowsAll) {
    qtEnv3();
    CallGraphScene scene;
    scene.loadGraph(
        {makeNode(1, "f1"), makeNode(2, "f2"), makeNode(3, "f3")}, {});
    scene.setNameFilter("");
    EXPECT_EQ(scene.visibleNodeCount(), 3);
}

TEST(CallGraphSceneTest, FocusModeHidesUnrelatedNodes) {
    qtEnv3();
    CallGraphScene scene;
    //  1→2→3  4 (isolated)
    scene.loadGraph(
        {makeNode(1,"a"), makeNode(2,"b"), makeNode(3,"c"), makeNode(4,"d")},
        {makeEdge(1,2), makeEdge(2,3)});
    scene.setFocusNode(2, 1); // 1-hop around node 2 → should show 1,2,3
    // Node 4 should not be in focus set
    EXPECT_LT(scene.visibleNodeCount(), 4);
}

TEST(CallGraphSceneTest, ClearFocusRestoresAll) {
    qtEnv3();
    CallGraphScene scene;
    scene.loadGraph(
        {makeNode(1,"a"), makeNode(2,"b"), makeNode(3,"c"), makeNode(4,"d")},
        {makeEdge(1,2), makeEdge(2,3)});
    scene.setFocusNode(1, 1);
    scene.clearFocus();
    EXPECT_FALSE(scene.isFocused());
    EXPECT_EQ(scene.visibleNodeCount(), 4);
}

TEST(CallGraphSceneTest, DotExportContainsFunctions) {
    qtEnv3();
    CallGraphScene scene;
    scene.loadGraph({makeNode(1, "main", 100), makeNode(2, "helper")},
                    {makeEdge(1, 2)});
    QString dot = scene.exportDot();
    EXPECT_TRUE(dot.contains("digraph"));
    EXPECT_TRUE(dot.contains("main"));
    EXPECT_TRUE(dot.contains("helper"));
    EXPECT_TRUE(dot.contains("->"));
}

TEST(CallGraphSceneTest, DotExportEmptyGraph) {
    qtEnv3();
    CallGraphScene scene;
    scene.loadGraph({}, {});
    QString dot = scene.exportDot();
    EXPECT_TRUE(dot.contains("digraph"));
}

TEST(CallGraphSceneTest, NodeClickedSignalExists) {
    qtEnv3();
    CallGraphScene scene;
    QSignalSpy spy(&scene, &CallGraphScene::nodeClicked);
    emit scene.nodeClicked(0xDEAD);
    EXPECT_EQ(spy.count(), 1);
}

// ═════════════════════════════════════════════════════════════════════════════
// CallGraphPanel tests
// ═════════════════════════════════════════════════════════════════════════════

TEST(CallGraphPanelTest, ConstructsWithoutCrash) {
    qtEnv3();
    EXPECT_NO_THROW({ CallGraphPanel panel; });
}

TEST(CallGraphPanelTest, LoadGraphDoesNotCrash) {
    qtEnv3();
    CallGraphPanel panel;
    EXPECT_NO_THROW(panel.loadGraph(
        {makeNode(1, "main"), makeNode(2, "sub")},
        {makeEdge(1, 2)}));
}

TEST(CallGraphPanelTest, ClearDoesNotCrash) {
    qtEnv3();
    CallGraphPanel panel;
    panel.loadGraph({makeNode(1, "f")}, {});
    EXPECT_NO_THROW(panel.clear());
}

TEST(CallGraphPanelTest, LegacySetCallGraphCompat) {
    qtEnv3();
    CallGraphPanel panel;
    EXPECT_NO_THROW(panel.setCallGraph({"funcA", "funcB"}, {makeEdge(0, 1)}));
}

TEST(CallGraphPanelTest, FunctionNavigationSignalExists) {
    qtEnv3();
    CallGraphPanel panel;
    QSignalSpy spy(&panel, &CallGraphPanel::functionNavigationRequested);
    emit panel.functionNavigationRequested(0x4000);
    EXPECT_EQ(spy.count(), 1);
}

TEST(CallGraphPanelTest, OnFunctionSelectedDoesNotCrash) {
    qtEnv3();
    CallGraphPanel panel;
    EXPECT_NO_THROW(panel.onFunctionSelected(0x1234, "main"));
}

// ═════════════════════════════════════════════════════════════════════════════
// CallGraphNodeItem tests
// ═════════════════════════════════════════════════════════════════════════════

TEST(CallGraphNodeItemTest, BoundingRectHasPositiveSize) {
    qtEnv3();
    auto n = makeNode(1, "test_func", 100);
    CallGraphNodeItem item(n, Qt::blue);
    EXPECT_GT(item.boundingRect().width(),  0);
    EXPECT_GT(item.boundingRect().height(), 0);
}

TEST(CallGraphNodeItemTest, LargerFunctionIsWider) {
    qtEnv3();
    auto n1 = makeNode(1, "small", 10);
    auto n2 = makeNode(2, "large", 10000);
    CallGraphNodeItem i1(n1, Qt::blue);
    CallGraphNodeItem i2(n2, Qt::blue);
    EXPECT_GE(i2.nodeRect().width(), i1.nodeRect().width());
}

TEST(CallGraphNodeItemTest, DimmedStateChanges) {
    qtEnv3();
    QGraphicsScene scene;
    auto n = makeNode(1, "f");
    auto* item = new CallGraphNodeItem(n, Qt::blue);
    scene.addItem(item);
    EXPECT_NO_THROW(item->setDimmed(true));
    EXPECT_NO_THROW(item->setDimmed(false));
}

// ═════════════════════════════════════════════════════════════════════════════
// SccSuperNodeItem tests
// ═════════════════════════════════════════════════════════════════════════════

TEST(SccSuperNodeItemTest, ConstructsWithMembers) {
    qtEnv3();
    EXPECT_NO_THROW({
        SccSuperNodeItem item({1u, 2u, 3u}, {"a", "b", "c"});
    });
}

TEST(SccSuperNodeItemTest, BoundingRectPositive) {
    qtEnv3();
    SccSuperNodeItem item({1u, 2u}, {"f1", "f2"});
    EXPECT_GT(item.boundingRect().width(),  0);
    EXPECT_GT(item.boundingRect().height(), 0);
}

TEST(SccSuperNodeItemTest, MembersReturned) {
    qtEnv3();
    SccSuperNodeItem item({10u, 20u, 30u}, {"a", "b", "c"});
    EXPECT_EQ(item.members().size(), 3u);
}

// ═════════════════════════════════════════════════════════════════════════════
// Data model defaults
// ═════════════════════════════════════════════════════════════════════════════

TEST(CallGraphDataModelTest, NodeDefaults) {
    CallGraphNode n;
    EXPECT_EQ(n.address, 0u);
    EXPECT_FALSE(n.isLibrary);
    EXPECT_EQ(n.moduleId, -1);
    EXPECT_EQ(n.instrCount, 0);
}

TEST(CallGraphDataModelTest, EdgeDefaults) {
    CallEdge e;
    EXPECT_EQ(e.callerAddress, 0u);
    EXPECT_EQ(e.calleeAddress, 0u);
    EXPECT_EQ(e.callCount,     1);
    EXPECT_TRUE(e.isDirect);
}

TEST(ClassInfoDataModelTest, Defaults) {
    ClassInfo ci;
    EXPECT_EQ(ci.vtableAddress, 0u);
    EXPECT_FALSE(ci.isAbstract);
    EXPECT_EQ(ci.methodCount, 0);
    EXPECT_TRUE(ci.name.isEmpty());
    EXPECT_TRUE(ci.bases.isEmpty());
    EXPECT_TRUE(ci.vtable.isEmpty());
}

TEST(VtableSlotDataModelTest, Defaults) {
    VtableSlot s;
    EXPECT_EQ(s.index, 0);
    EXPECT_EQ(s.funcAddress, 0u);
    EXPECT_FALSE(s.isPure);
    EXPECT_FALSE(s.isOverride);
}
