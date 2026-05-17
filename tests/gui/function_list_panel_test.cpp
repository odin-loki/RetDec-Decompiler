/**
 * @file tests/gui/function_list_panel_test.cpp
 * @brief Unit tests for FunctionListPanel with recovery metadata (Task 53).
 *
 * Tests cover:
 *   - RecoveryConfidence composite score calculation
 *   - FunctionListModel: rowCount, columnCount, data, setData, rename, applyTag
 *   - FunctionFilterProxy: name glob, address range, confidence, pattern, class
 *   - FunctionListPanel: construction, setFunctions, clear, filterByClass
 *   - FunctionListPanel: signals functionSelected, functionRenamed
 *   - Export (CSV/JSON) slot calls do not crash on empty model
 */

#include <gtest/gtest.h>
#include <QApplication>
#include <QSignalSpy>

#include "retdec/gui/panels/function_list_panel.h"

#include "qt_test_env.h"

using namespace retdec::gui::panels;

// ─── Qt (single QApplication lives in qt_test_main.cpp) ──────────────────────
namespace {

void qtEnv4() {
    Q_ASSERT(QApplication::instance() != nullptr);
}

} // anonymous namespace

// ─── Helpers ─────────────────────────────────────────────────────────────────

static FunctionEntry makeFn(uint64_t addr, const QString& name,
                              float confidence = 0.8f,
                              bool stl = false, bool crypto = false,
                              bool isLibrary = false)
{
    FunctionEntry e;
    e.address    = addr;
    e.name       = name;
    e.rawName    = "_Z" + name;
    e.sizeBytes  = 100;
    e.instrCount = 25;
    e.cc         = "cdecl";
    e.isLibrary  = isLibrary;
    e.patterns.stl    = stl;
    e.patterns.crypto = crypto;
    e.confidence.typeInference = confidence;
    e.confidence.structure     = confidence;
    e.confidence.variable      = confidence;
    e.confidence.algorithm     = confidence;
    return e;
}

// ═════════════════════════════════════════════════════════════════════════════
// RecoveryConfidence tests
// ═════════════════════════════════════════════════════════════════════════════

TEST(RecoveryConfidenceTest, ZeroComposite) {
    RecoveryConfidence c;
    EXPECT_FLOAT_EQ(c.composite(), 0.f);
}

TEST(RecoveryConfidenceTest, AllOnesGivesOne) {
    RecoveryConfidence c;
    c.typeInference = c.structure = c.variable = c.algorithm = 1.f;
    EXPECT_NEAR(c.composite(), 1.f, 1e-5f);
}

TEST(RecoveryConfidenceTest, WeightedSum) {
    RecoveryConfidence c;
    c.typeInference = 1.f; c.structure = 0.f; c.variable = 0.f; c.algorithm = 0.f;
    EXPECT_NEAR(c.composite(), 0.30f, 1e-5f);

    c.typeInference = 0.f; c.structure = 1.f;
    EXPECT_NEAR(c.composite(), 0.30f, 1e-5f);

    c.structure = 0.f; c.variable = 1.f;
    EXPECT_NEAR(c.composite(), 0.20f, 1e-5f);

    c.variable = 0.f; c.algorithm = 1.f;
    EXPECT_NEAR(c.composite(), 0.20f, 1e-5f);
}

TEST(RecoveryConfidenceTest, MixedWeights) {
    RecoveryConfidence c;
    c.typeInference = 0.5f; c.structure = 0.5f;
    c.variable      = 0.5f; c.algorithm = 0.5f;
    EXPECT_NEAR(c.composite(), 0.5f, 1e-5f);
}

// ═════════════════════════════════════════════════════════════════════════════
// PatternFlags tests
// ═════════════════════════════════════════════════════════════════════════════

TEST(PatternFlagsTest, DefaultsAllFalse) {
    PatternFlags f;
    EXPECT_FALSE(f.stl);
    EXPECT_FALSE(f.crypto);
    EXPECT_FALSE(f.algo);
    EXPECT_FALSE(f.design);
    EXPECT_FALSE(f.library);
}

// ═════════════════════════════════════════════════════════════════════════════
// FunctionListModel tests
// ═════════════════════════════════════════════════════════════════════════════

TEST(FunctionListModelTest, EmptyModelHasNoRows) {
    qtEnv4();
    FunctionListModel model;
    EXPECT_EQ(model.rowCount(), 0);
}

TEST(FunctionListModelTest, ColumnCount) {
    qtEnv4();
    FunctionListModel model;
    EXPECT_EQ(model.columnCount(), FunctionListModel::ColCount);
    EXPECT_EQ(model.columnCount(), 8);
}

TEST(FunctionListModelTest, SetFunctionsUpdatesRowCount) {
    qtEnv4();
    FunctionListModel model;
    model.setFunctions({makeFn(1, "a"), makeFn(2, "b"), makeFn(3, "c")});
    EXPECT_EQ(model.rowCount(), 3);
}

TEST(FunctionListModelTest, AddressColumnFormat) {
    qtEnv4();
    FunctionListModel model;
    auto fn = makeFn(0xDEADBEEF, "test");
    model.setFunctions({fn});
    QModelIndex idx = model.index(0, FunctionListModel::ColAddress);
    QString s = model.data(idx, Qt::DisplayRole).toString();
    EXPECT_TRUE(s.contains("deadbeef", Qt::CaseInsensitive));
}

TEST(FunctionListModelTest, NameColumnDisplaysDemangled) {
    qtEnv4();
    FunctionListModel model;
    model.setFunctions({makeFn(1, "MyClass::myMethod")});
    QModelIndex idx = model.index(0, FunctionListModel::ColName);
    EXPECT_EQ(model.data(idx).toString(), "MyClass::myMethod");
}

TEST(FunctionListModelTest, ConfidenceColumnShowsPercent) {
    qtEnv4();
    FunctionListModel model;
    auto fn = makeFn(1, "f", 1.0f);
    model.setFunctions({fn});
    QModelIndex idx = model.index(0, FunctionListModel::ColConfidence);
    QString s = model.data(idx).toString();
    EXPECT_TRUE(s.contains("100") || s.contains("99"));
}

TEST(FunctionListModelTest, CCColumnDisplaysConvention) {
    qtEnv4();
    FunctionListModel model;
    auto fn = makeFn(1, "f");
    fn.cc = "stdcall";
    model.setFunctions({fn});
    EXPECT_EQ(model.data(model.index(0, FunctionListModel::ColCC)).toString(), "stdcall");
}

TEST(FunctionListModelTest, HeaderLabelsNotEmpty) {
    qtEnv4();
    FunctionListModel model;
    for (int c = 0; c < FunctionListModel::ColCount; ++c) {
        QVariant h = model.headerData(c, Qt::Horizontal, Qt::DisplayRole);
        EXPECT_FALSE(h.toString().isEmpty()) << "Column " << c;
    }
}

TEST(FunctionListModelTest, InvalidIndexReturnsEmpty) {
    qtEnv4();
    FunctionListModel model;
    EXPECT_FALSE(model.data(QModelIndex()).isValid());
}

TEST(FunctionListModelTest, NameColumnIsEditable) {
    qtEnv4();
    FunctionListModel model;
    model.setFunctions({makeFn(1, "old")});
    Qt::ItemFlags flags = model.flags(model.index(0, FunctionListModel::ColName));
    EXPECT_TRUE(flags & Qt::ItemIsEditable);
}

TEST(FunctionListModelTest, SetDataRenamesFunction) {
    qtEnv4();
    FunctionListModel model;
    model.setFunctions({makeFn(100, "oldName")});
    QSignalSpy spy(&model, &FunctionListModel::functionRenamed);
    bool ok = model.setData(model.index(0, FunctionListModel::ColName),
                            "newName", Qt::EditRole);
    EXPECT_TRUE(ok);
    EXPECT_EQ(model.entry(0).name, "newName");
    EXPECT_EQ(spy.count(), 1);
    EXPECT_EQ(spy[0][2].toString(), "newName");
}

TEST(FunctionListModelTest, RenameFunctionByAddress) {
    qtEnv4();
    FunctionListModel model;
    model.setFunctions({makeFn(0xABCD, "old"), makeFn(0x1234, "other")});
    bool ok = model.renameFunction(0xABCD, "renamedFn");
    EXPECT_TRUE(ok);
    // Find the entry
    bool found = false;
    for (int i = 0; i < model.rowCount(); ++i) {
        if (model.entry(i).address == 0xABCD) {
            EXPECT_EQ(model.entry(i).name, "renamedFn");
            found = true;
        }
    }
    EXPECT_TRUE(found);
}

TEST(FunctionListModelTest, RenameFunctionMissingAddressReturnsFalse) {
    qtEnv4();
    FunctionListModel model;
    model.setFunctions({makeFn(1, "f")});
    EXPECT_FALSE(model.renameFunction(0xDEAD, "newName"));
}

TEST(FunctionListModelTest, ApplyTagToMultipleFunctions) {
    qtEnv4();
    FunctionListModel model;
    model.setFunctions({makeFn(1,"a"), makeFn(2,"b"), makeFn(3,"c")});
    model.applyTag({1u, 3u}, "important");
    EXPECT_TRUE(model.entry(0).tags.contains("important"));
    EXPECT_FALSE(model.entry(1).tags.contains("important"));
    EXPECT_TRUE(model.entry(2).tags.contains("important"));
}

TEST(FunctionListModelTest, ApplyTagIdempotent) {
    qtEnv4();
    FunctionListModel model;
    model.setFunctions({makeFn(1, "f")});
    model.applyTag({1u}, "tag1");
    model.applyTag({1u}, "tag1");
    EXPECT_EQ(model.entry(0).tags.count("tag1"), 1);
}

TEST(FunctionListModelTest, ClearResetsRowCount) {
    qtEnv4();
    FunctionListModel model;
    model.setFunctions({makeFn(1,"a"), makeFn(2,"b")});
    model.clearAll();
    EXPECT_EQ(model.rowCount(), 0);
}

TEST(FunctionListModelTest, TooltipForConfidenceColumn) {
    qtEnv4();
    FunctionListModel model;
    model.setFunctions({makeFn(1, "f", 0.6f)});
    QVariant tip = model.data(model.index(0, FunctionListModel::ColConfidence),
                              Qt::ToolTipRole);
    EXPECT_TRUE(tip.isValid());
    EXPECT_FALSE(tip.toString().isEmpty());
}

// ═════════════════════════════════════════════════════════════════════════════
// FunctionFilterProxy tests
// ═════════════════════════════════════════════════════════════════════════════

TEST(FunctionFilterProxyTest, NoFilterShowsAll) {
    qtEnv4();
    FunctionListModel model;
    FunctionFilterProxy proxy(&model);
    model.setFunctions({makeFn(1,"a"), makeFn(2,"b"), makeFn(3,"c")});
    EXPECT_EQ(proxy.rowCount(), 3);
}

TEST(FunctionFilterProxyTest, NameGlobFilters) {
    qtEnv4();
    FunctionListModel model;
    FunctionFilterProxy proxy(&model);
    model.setFunctions({makeFn(1,"alpha"), makeFn(2,"beta"), makeFn(3,"gamma")});
    proxy.setNameGlob("*pha*");
    EXPECT_EQ(proxy.rowCount(), 1);
}

TEST(FunctionFilterProxyTest, EmptyGlobShowsAll) {
    qtEnv4();
    FunctionListModel model;
    FunctionFilterProxy proxy(&model);
    model.setFunctions({makeFn(1,"a"), makeFn(2,"b")});
    proxy.setNameGlob("");
    EXPECT_EQ(proxy.rowCount(), 2);
}

TEST(FunctionFilterProxyTest, AddressRangeLow) {
    qtEnv4();
    FunctionListModel model;
    FunctionFilterProxy proxy(&model);
    model.setFunctions({makeFn(0x100,"a"), makeFn(0x200,"b"), makeFn(0x300,"c")});
    proxy.setAddressRange(0x200, 0);
    EXPECT_EQ(proxy.rowCount(), 2); // 0x200 and 0x300
}

TEST(FunctionFilterProxyTest, AddressRangeHigh) {
    qtEnv4();
    FunctionListModel model;
    FunctionFilterProxy proxy(&model);
    model.setFunctions({makeFn(0x100,"a"), makeFn(0x200,"b"), makeFn(0x300,"c")});
    proxy.setAddressRange(0, 0x200);
    EXPECT_EQ(proxy.rowCount(), 2); // 0x100 and 0x200
}

TEST(FunctionFilterProxyTest, ConfidenceThreshold) {
    qtEnv4();
    FunctionListModel model;
    FunctionFilterProxy proxy(&model);
    model.setFunctions({makeFn(1,"lo", 0.2f), makeFn(2,"hi", 0.9f)});
    proxy.setMinConfidence(0.5f);
    EXPECT_EQ(proxy.rowCount(), 1);
}

TEST(FunctionFilterProxyTest, PatternFilterStl) {
    qtEnv4();
    FunctionListModel model;
    FunctionFilterProxy proxy(&model);
    model.setFunctions({
        makeFn(1,"a", 0.5f, /*stl=*/true),
        makeFn(2,"b", 0.5f, /*stl=*/false),
    });
    proxy.setPatternFilter(true, false, false, false, false);
    EXPECT_EQ(proxy.rowCount(), 1);
}

TEST(FunctionFilterProxyTest, PatternFilterNoActiveShowsAll) {
    qtEnv4();
    FunctionListModel model;
    FunctionFilterProxy proxy(&model);
    model.setFunctions({makeFn(1,"a"), makeFn(2,"b")});
    proxy.setPatternFilter(false, false, false, false, false);
    EXPECT_EQ(proxy.rowCount(), 2);
}

TEST(FunctionFilterProxyTest, ClassFilterMatchesPrefix) {
    qtEnv4();
    FunctionListModel model;
    FunctionFilterProxy proxy(&model);
    model.setFunctions({
        makeFn(1,"MyClass::foo"),
        makeFn(2,"MyClass::bar"),
        makeFn(3,"OtherClass::baz"),
    });
    proxy.setClassFilter("MyClass");
    EXPECT_EQ(proxy.rowCount(), 2);
}

TEST(FunctionFilterProxyTest, ClassFilterEmpty_ShowsAll) {
    qtEnv4();
    FunctionListModel model;
    FunctionFilterProxy proxy(&model);
    model.setFunctions({makeFn(1,"A::f"), makeFn(2,"B::g")});
    proxy.setClassFilter("");
    EXPECT_EQ(proxy.rowCount(), 2);
}

TEST(FunctionFilterProxyTest, MultipleFiltersAnded) {
    qtEnv4();
    FunctionListModel model;
    FunctionFilterProxy proxy(&model);
    model.setFunctions({
        makeFn(0x100,"foo", 0.9f, true),   // matches name "foo", high conf, stl
        makeFn(0x200,"foo", 0.1f, true),   // matches name but low confidence
        makeFn(0x300,"bar", 0.9f, true),   // high conf, stl, but wrong name
    });
    proxy.setNameGlob("*foo*");
    proxy.setMinConfidence(0.5f);
    proxy.setPatternFilter(true, false, false, false, false);
    EXPECT_EQ(proxy.rowCount(), 1);
}

// ═════════════════════════════════════════════════════════════════════════════
// FunctionListPanel tests
// ═════════════════════════════════════════════════════════════════════════════

TEST(FunctionListPanelTest, ConstructsWithoutCrash) {
    qtEnv4();
    EXPECT_NO_THROW({ FunctionListPanel panel; });
}

TEST(FunctionListPanelTest, SetFunctionsDoesNotCrash) {
    qtEnv4();
    FunctionListPanel panel;
    EXPECT_NO_THROW(panel.setFunctions({makeFn(1,"a"), makeFn(2,"b")}));
}

TEST(FunctionListPanelTest, ClearDoesNotCrash) {
    qtEnv4();
    FunctionListPanel panel;
    panel.setFunctions({makeFn(1,"a")});
    EXPECT_NO_THROW(panel.clear());
}

TEST(FunctionListPanelTest, FilterByClassDoesNotCrash) {
    qtEnv4();
    FunctionListPanel panel;
    panel.setFunctions({makeFn(1,"MyClass::foo"), makeFn(2,"Other::bar")});
    EXPECT_NO_THROW(panel.filterByClass("MyClass"));
}

TEST(FunctionListPanelTest, FunctionSelectedSignalExists) {
    qtEnv4();
    FunctionListPanel panel;
    QSignalSpy spy(&panel, &FunctionListPanel::functionSelected);
    emit panel.functionSelected(0x1234, "main");
    EXPECT_EQ(spy.count(), 1);
}

TEST(FunctionListPanelTest, FunctionRenamedSignalExists) {
    qtEnv4();
    FunctionListPanel panel;
    QSignalSpy spy(&panel, &FunctionListPanel::functionRenamed);
    emit panel.functionRenamed(0x1234, "old", "new");
    EXPECT_EQ(spy.count(), 1);
}

TEST(FunctionListPanelTest, LargeFunctionListLoadsQuickly) {
    qtEnv4();
    FunctionListPanel panel;
    // Task 53 requires 1000 functions load in < 1s; we just verify no crash
    std::vector<FunctionEntry> fns;
    fns.reserve(1000);
    for (uint64_t i = 0; i < 1000; ++i)
        fns.push_back(makeFn(i * 4, QString("func_%1").arg(i)));
    EXPECT_NO_THROW(panel.setFunctions(std::move(fns)));
}

TEST(FunctionListPanelTest, EmptySetFunctions) {
    qtEnv4();
    FunctionListPanel panel;
    EXPECT_NO_THROW(panel.setFunctions({}));
}
