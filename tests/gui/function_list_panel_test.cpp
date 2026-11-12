/**
 * @file tests/gui/function_list_panel_test.cpp
 * @brief Function list: the Qt FunctionListPanel tests ported to the model
 *        (FunctionList / FunctionListProxy) and to the FuseUI panel, plus
 *        headless UI tests that drive the panel like a user.
 * @copyright (c) 2026 Odin Loch trading as Imortek
 */

#include "retdec/gui/model/funclist_export.h"
#include "retdec/gui/model/funclist_filter.h"
#include "retdec/gui/model/funclist_model.h"
#include "retdec/gui/ui/function_list_panel.h"

#include "table_panels_test_util.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <sstream>

using namespace retdec::gui::model;
using retdec::gui::ui::FunctionListPanel;
using retdec_gui_tp_test::Headless;
namespace w = fuse::ui::widgets;

namespace {

FunctionEntry makeFn(
	uint64_t addr,
	const std::string& name,
	float confidence = 0.8f,
	bool stl = false,
	bool crypto = false,
	bool isLibrary = false)
{
	FunctionEntry e;
	e.address = addr;
	e.name = name;
	e.rawName = "_Z" + name;
	e.sizeBytes = 100;
	e.instrCount = 25;
	e.cc = "cdecl";
	e.isLibrary = isLibrary;
	e.patterns.stl = stl;
	e.patterns.crypto = crypto;
	e.confidence.typeInference = confidence;
	e.confidence.structure = confidence;
	e.confidence.variable = confidence;
	e.confidence.algorithm = confidence;
	return e;
}

std::string readFile(const std::filesystem::path& p)
{
	std::ifstream f(p, std::ios::binary);
	std::stringstream ss;
	ss << f.rdbuf();
	return ss.str();
}

} // namespace

// ═════════════════════════════════════════════════════════════════════════════
// Ported: RecoveryConfidence / PatternFlags
// ═════════════════════════════════════════════════════════════════════════════

TEST(RecoveryConfidenceTest, ZeroComposite)
{
	RecoveryConfidence c;
	EXPECT_FLOAT_EQ(c.composite(), 0.f);
}

TEST(RecoveryConfidenceTest, AllOnesGivesOne)
{
	RecoveryConfidence c;
	c.typeInference = c.structure = c.variable = c.algorithm = 1.f;
	EXPECT_NEAR(c.composite(), 1.f, 1e-5f);
}

TEST(RecoveryConfidenceTest, WeightedSum)
{
	RecoveryConfidence c;
	c.typeInference = 1.f;
	EXPECT_NEAR(c.composite(), 0.30f, 1e-5f);
	c.typeInference = 0.f;
	c.structure = 1.f;
	EXPECT_NEAR(c.composite(), 0.30f, 1e-5f);
	c.structure = 0.f;
	c.variable = 1.f;
	EXPECT_NEAR(c.composite(), 0.20f, 1e-5f);
	c.variable = 0.f;
	c.algorithm = 1.f;
	EXPECT_NEAR(c.composite(), 0.20f, 1e-5f);
}

TEST(RecoveryConfidenceTest, MixedWeights)
{
	RecoveryConfidence c;
	c.typeInference = c.structure = c.variable = c.algorithm = 0.5f;
	EXPECT_NEAR(c.composite(), 0.5f, 1e-5f);
}

TEST(PatternFlagsTest, DefaultsAllFalse)
{
	PatternFlags f;
	EXPECT_FALSE(f.stl);
	EXPECT_FALSE(f.crypto);
	EXPECT_FALSE(f.algo);
	EXPECT_FALSE(f.design);
	EXPECT_FALSE(f.library);
}

// ═════════════════════════════════════════════════════════════════════════════
// Ported: FunctionListModel -> FunctionList + column text
// ═════════════════════════════════════════════════════════════════════════════

TEST(FunctionListModelTest, EmptyModelHasNoRows)
{
	FunctionList model;
	EXPECT_EQ(model.size(), 0u);
}

TEST(FunctionListModelTest, ColumnCount)
{
	EXPECT_EQ(FnColCount, 8);
}

TEST(FunctionListModelTest, SetFunctionsUpdatesRowCount)
{
	FunctionList model;
	model.setFunctions({makeFn(1, "a"), makeFn(2, "b"), makeFn(3, "c")});
	EXPECT_EQ(model.size(), 3u);
}

TEST(FunctionListModelTest, AddressColumnFormat)
{
	const std::string s = functionCellText(makeFn(0xDEADBEEF, "test"), FnColAddress);
	EXPECT_NE(s.find("deadbeef"), std::string::npos);
	EXPECT_EQ(s, "0x00000000deadbeef");
}

TEST(FunctionListModelTest, NameColumnDisplaysDemangled)
{
	EXPECT_EQ(functionCellText(makeFn(1, "MyClass::myMethod"), FnColName), "MyClass::myMethod");
	EXPECT_EQ(functionCellText(makeFn(1, "MyClass::myMethod"), FnColName, false), "_ZMyClass::myMethod");
}

TEST(FunctionListModelTest, ConfidenceColumnShowsPercent)
{
	const std::string s = functionCellText(makeFn(1, "f", 1.0f), FnColConfidence);
	EXPECT_TRUE(s.find("100") != std::string::npos || s.find("99") != std::string::npos);
	EXPECT_NE(s.find('%'), std::string::npos);
}

TEST(FunctionListModelTest, CCColumnDisplaysConvention)
{
	auto fn = makeFn(1, "f");
	fn.cc = "stdcall";
	EXPECT_EQ(functionCellText(fn, FnColCC), "stdcall");
	fn.cc.clear();
	EXPECT_EQ(functionCellText(fn, FnColCC), "\xE2\x80\x94");
}

TEST(FunctionListModelTest, HeaderLabelsNotEmpty)
{
	for (int c = 0; c < FnColCount; ++c)
		EXPECT_FALSE(functionColumnHeader(c).empty()) << "Column " << c;
}

TEST(FunctionListModelTest, InvalidIndexReturnsEmpty)
{
	EXPECT_TRUE(functionCellText(makeFn(1, "f"), 99).empty());
	EXPECT_TRUE(functionColumnHeader(99).empty());
}

TEST(FunctionListModelTest, NameColumnIsEditable)
{
	EXPECT_TRUE(functionColumnEditable(FnColName));
	EXPECT_TRUE(functionColumnEditable(FnColNotes));
	EXPECT_FALSE(functionColumnEditable(FnColAddress));
}

TEST(FunctionListModelTest, SetDataRenamesFunction)
{
	FunctionList model;
	model.setFunctions({makeFn(100, "oldName")});
	int count = 0;
	std::string lastNew;
	model.onRenamed = [&](uint64_t, const std::string&, const std::string& n) {
		++count;
		lastNew = n;
	};
	EXPECT_TRUE(model.setName(0, "newName"));
	EXPECT_EQ(model.entry(0).name, "newName");
	EXPECT_EQ(count, 1);
	EXPECT_EQ(lastNew, "newName");
}

TEST(FunctionListModelTest, RenameFunctionByAddress)
{
	FunctionList model;
	model.setFunctions({makeFn(0xABCD, "old"), makeFn(0x1234, "other")});
	EXPECT_TRUE(model.renameFunction(0xABCD, "renamedFn"));
	bool found = false;
	for (std::size_t i = 0; i < model.size(); ++i)
	{
		if (model.entry(i).address == 0xABCD)
		{
			EXPECT_EQ(model.entry(i).name, "renamedFn");
			found = true;
		}
	}
	EXPECT_TRUE(found);
}

TEST(FunctionListModelTest, RenameFunctionMissingAddressReturnsFalse)
{
	FunctionList model;
	model.setFunctions({makeFn(1, "f")});
	EXPECT_FALSE(model.renameFunction(0xDEAD, "newName"));
}

TEST(FunctionListModelTest, ApplyTagToMultipleFunctions)
{
	FunctionList model;
	model.setFunctions({makeFn(1, "a"), makeFn(2, "b"), makeFn(3, "c")});
	model.applyTag({1u, 3u}, "important");
	auto has = [&](std::size_t r) {
		const auto& t = model.entry(r).tags;
		return std::find(t.begin(), t.end(), "important") != t.end();
	};
	EXPECT_TRUE(has(0));
	EXPECT_FALSE(has(1));
	EXPECT_TRUE(has(2));
}

TEST(FunctionListModelTest, ApplyTagIdempotent)
{
	FunctionList model;
	model.setFunctions({makeFn(1, "f")});
	model.applyTag({1u}, "tag1");
	model.applyTag({1u}, "tag1");
	EXPECT_EQ(std::count(model.entry(0).tags.begin(), model.entry(0).tags.end(), "tag1"), 1);
}

TEST(FunctionListModelTest, ClearResetsRowCount)
{
	FunctionList model;
	model.setFunctions({makeFn(1, "a"), makeFn(2, "b")});
	model.clearAll();
	EXPECT_EQ(model.size(), 0u);
}

TEST(FunctionListModelTest, TooltipForConfidenceColumn)
{
	const std::string tip = functionToolTip(makeFn(1, "f", 0.6f), FnColConfidence);
	EXPECT_FALSE(tip.empty());
	EXPECT_NE(tip.find("Composite:"), std::string::npos);
}

// ═════════════════════════════════════════════════════════════════════════════
// Ported: FunctionFilterProxy -> FunctionListProxy
// ═════════════════════════════════════════════════════════════════════════════

TEST(FunctionFilterProxyTest, NoFilterShowsAll)
{
	FunctionList model;
	FunctionListProxy proxy(&model);
	model.setFunctions({makeFn(1, "a"), makeFn(2, "b"), makeFn(3, "c")});
	EXPECT_EQ(proxy.rowCount(), 3u);
}

TEST(FunctionFilterProxyTest, NameGlobFilters)
{
	FunctionList model;
	FunctionListProxy proxy(&model);
	model.setFunctions({makeFn(1, "alpha"), makeFn(2, "beta"), makeFn(3, "gamma")});
	proxy.setNameGlob("*pha*");
	EXPECT_EQ(proxy.rowCount(), 1u);
}

TEST(FunctionFilterProxyTest, EmptyGlobShowsAll)
{
	FunctionList model;
	FunctionListProxy proxy(&model);
	model.setFunctions({makeFn(1, "a"), makeFn(2, "b")});
	proxy.setNameGlob("");
	EXPECT_EQ(proxy.rowCount(), 2u);
}

TEST(FunctionFilterProxyTest, AddressRangeLow)
{
	FunctionList model;
	FunctionListProxy proxy(&model);
	model.setFunctions({makeFn(0x100, "a"), makeFn(0x200, "b"), makeFn(0x300, "c")});
	proxy.setAddressRange(0x200, 0);
	EXPECT_EQ(proxy.rowCount(), 2u);
}

TEST(FunctionFilterProxyTest, AddressRangeHigh)
{
	FunctionList model;
	FunctionListProxy proxy(&model);
	model.setFunctions({makeFn(0x100, "a"), makeFn(0x200, "b"), makeFn(0x300, "c")});
	proxy.setAddressRange(0, 0x200);
	EXPECT_EQ(proxy.rowCount(), 2u);
}

TEST(FunctionFilterProxyTest, ConfidenceThreshold)
{
	FunctionList model;
	FunctionListProxy proxy(&model);
	model.setFunctions({makeFn(1, "lo", 0.2f), makeFn(2, "hi", 0.9f)});
	proxy.setMinConfidence(0.5f);
	EXPECT_EQ(proxy.rowCount(), 1u);
}

TEST(FunctionFilterProxyTest, PatternFilterStl)
{
	FunctionList model;
	FunctionListProxy proxy(&model);
	model.setFunctions({makeFn(1, "a", 0.5f, true), makeFn(2, "b", 0.5f, false)});
	proxy.setPatternFilter(true, false, false, false, false);
	EXPECT_EQ(proxy.rowCount(), 1u);
}

TEST(FunctionFilterProxyTest, PatternFilterNoActiveShowsAll)
{
	FunctionList model;
	FunctionListProxy proxy(&model);
	model.setFunctions({makeFn(1, "a"), makeFn(2, "b")});
	proxy.setPatternFilter(false, false, false, false, false);
	EXPECT_EQ(proxy.rowCount(), 2u);
}

TEST(FunctionFilterProxyTest, ClassFilterMatchesPrefix)
{
	FunctionList model;
	FunctionListProxy proxy(&model);
	model.setFunctions({makeFn(1, "MyClass::foo"), makeFn(2, "MyClass::bar"), makeFn(3, "OtherClass::baz")});
	proxy.setClassFilter("MyClass");
	EXPECT_EQ(proxy.rowCount(), 2u);
}

TEST(FunctionFilterProxyTest, ClassFilterEmpty_ShowsAll)
{
	FunctionList model;
	FunctionListProxy proxy(&model);
	model.setFunctions({makeFn(1, "A::f"), makeFn(2, "B::g")});
	proxy.setClassFilter("");
	EXPECT_EQ(proxy.rowCount(), 2u);
}

TEST(FunctionFilterProxyTest, MultipleFiltersAnded)
{
	FunctionList model;
	FunctionListProxy proxy(&model);
	model.setFunctions({
		makeFn(0x100, "foo", 0.9f, true),
		makeFn(0x200, "foo", 0.1f, true),
		makeFn(0x300, "bar", 0.9f, true),
	});
	proxy.setNameGlob("*foo*");
	proxy.setMinConfidence(0.5f);
	proxy.setPatternFilter(true, false, false, false, false);
	EXPECT_EQ(proxy.rowCount(), 1u);
}

// ═════════════════════════════════════════════════════════════════════════════
// Ported: FunctionListPanel (headless FuseUI)
// ═════════════════════════════════════════════════════════════════════════════

TEST(FunctionListPanelTest, ConstructsWithoutCrash)
{
	Headless h;
	EXPECT_NO_THROW({ FunctionListPanel panel(h.ui, h.ui.root()); });
}

TEST(FunctionListPanelTest, DestroyRemovesNodes)
{
	Headless h;
	w::overlayLayer(h.ui);
	h.frame();
	const auto before = h.ui.nodeCount();
	{
		FunctionListPanel panel(h.ui, h.ui.root());
		h.frame();
		EXPECT_GT(h.ui.nodeCount(), before);
	}
	h.frame();
	EXPECT_EQ(h.ui.nodeCount(), before);
}

TEST(FunctionListPanelTest, SetFunctionsDoesNotCrash)
{
	Headless h;
	FunctionListPanel panel(h.ui, h.ui.root());
	EXPECT_NO_THROW(panel.setFunctions({makeFn(1, "a"), makeFn(2, "b")}));
	EXPECT_EQ(panel.visibleRowCount(), 2u);
}

TEST(FunctionListPanelTest, ClearDoesNotCrash)
{
	Headless h;
	FunctionListPanel panel(h.ui, h.ui.root());
	panel.setFunctions({makeFn(1, "a")});
	panel.setNameFilterText("zzz");
	EXPECT_NO_THROW(panel.clear());
	EXPECT_EQ(panel.visibleRowCount(), 0u);
	EXPECT_EQ(panel.nameFilterText(), "");
}

TEST(FunctionListPanelTest, FilterByClassDoesNotCrash)
{
	Headless h;
	FunctionListPanel panel(h.ui, h.ui.root());
	panel.setFunctions({makeFn(1, "MyClass::foo"), makeFn(2, "Other::bar")});
	EXPECT_NO_THROW(panel.filterByClass("MyClass"));
	EXPECT_EQ(panel.visibleRowCount(), 1u);
	panel.filterByClass("");
	EXPECT_EQ(panel.visibleRowCount(), 2u);
}

TEST(FunctionListPanelTest, FunctionSelectedSignalExists)
{
	Headless h;
	FunctionListPanel panel(h.ui, h.ui.root());
	panel.setFunctions({makeFn(0x1234, "main")});
	h.frame();
	int count = 0;
	uint64_t addr = 0;
	std::string name;
	panel.onFunctionActivated = [&](uint64_t a, const std::string& n) {
		++count;
		addr = a;
		name = n;
	};
	panel.selectFunction(0x1234);
	EXPECT_EQ(count, 1);
	EXPECT_EQ(addr, 0x1234u);
	EXPECT_EQ(name, "main");
	ASSERT_TRUE(panel.selectedEntry().has_value());
	EXPECT_EQ(panel.selectedEntry()->address, 0x1234u);
}

TEST(FunctionListPanelTest, FunctionRenamedSignalExists)
{
	Headless h;
	FunctionListPanel panel(h.ui, h.ui.root());
	panel.setFunctions({makeFn(0x1234, "old")});
	h.frame();
	int count = 0;
	std::string o, n;
	panel.onFunctionRenamed = [&](uint64_t, const std::string& a, const std::string& b) {
		++count;
		o = a;
		n = b;
	};
	panel.triggerAction(FunctionListPanel::Action::Rename, 0x1234);
	ASSERT_TRUE(panel.promptOpen());
	panel.acceptPrompt("new");
	EXPECT_EQ(count, 1);
	EXPECT_EQ(o, "old");
	EXPECT_EQ(n, "new");
	EXPECT_FALSE(panel.promptOpen());
}

TEST(FunctionListPanelTest, LargeFunctionListLoadsQuickly)
{
	Headless h;
	FunctionListPanel panel(h.ui, h.ui.root());
	std::vector<FunctionEntry> fns;
	fns.reserve(1000);
	for (uint64_t i = 0; i < 1000; ++i)
		fns.push_back(makeFn(i * 4, "func_" + std::to_string(i)));
	EXPECT_NO_THROW(panel.setFunctions(std::move(fns)));
	h.frame();
	EXPECT_EQ(panel.visibleRowCount(), 1000u);
	EXPECT_EQ(panel.statusText(), "1000 functions");
}

TEST(FunctionListPanelTest, EmptySetFunctions)
{
	Headless h;
	FunctionListPanel panel(h.ui, h.ui.root());
	EXPECT_NO_THROW(panel.setFunctions({}));
	h.frame();
	EXPECT_EQ(panel.statusText(), "No functions \xE2\x80\x94 run Analysis (F5)");
	EXPECT_TRUE(h.ui.computed(panel.emptyStateNode()).visible);
}

// ═════════════════════════════════════════════════════════════════════════════
// Headless UI: drive the panel like a user
// ═════════════════════════════════════════════════════════════════════════════

namespace {

struct PanelFixture
{
	Headless h;
	FunctionListPanel panel{h.ui, h.ui.root()};
	PanelFixture()
	{
		auto a = makeFn(0x401000, "alpha", 0.9f, true);
		auto b = makeFn(0x402000, "beta", 0.5f, false, true);
		auto c = makeFn(0x403000, "gamma", 0.2f);
		c.isLibrary = true;
		c.patterns.library = true;
		b.notes = "check me";
		b.tags = {"net"};
		b.signature = "int beta(char*)";
		panel.setFunctions({c, a, b});
		h.frame();
	}
};

} // namespace

TEST(FunctionListPanelUiTest, ShowsRowsSortedByAddressWithStatus)
{
	PanelFixture f;
	ASSERT_EQ(f.panel.visibleRowCount(), 3u);
	EXPECT_EQ(f.panel.cellText(0, FnColName), "alpha");
	EXPECT_EQ(f.panel.cellText(2, FnColName), "gamma");
	EXPECT_EQ(f.panel.statusText(), "3 functions");
	EXPECT_EQ(f.h.ui.text(f.panel.statusNode()), "3 functions");
	EXPECT_FALSE(f.h.ui.computed(f.panel.emptyStateNode()).visible);
	EXPECT_EQ(f.panel.cellText(1, FnColNotes), "check me  [net]");
	EXPECT_EQ(f.panel.cellText(0, FnColSize), "100 B");
}

TEST(FunctionListPanelUiTest, TypingInFilterBoxFilters)
{
	PanelFixture f;
	f.h.typeInto(f.panel.nameFilterNode(), "*ph*");
	EXPECT_EQ(f.panel.nameFilterText(), "*ph*");
	ASSERT_EQ(f.panel.visibleRowCount(), 1u);
	EXPECT_EQ(f.panel.cellText(0, FnColName), "alpha");
	EXPECT_EQ(f.panel.statusText(), "1 / 3 functions");
	// Whole-name wildcard match: "be" alone matches nothing, "BE*" (any case) does.
	f.h.typeInto(f.panel.nameFilterNode(), "be");
	EXPECT_EQ(f.panel.visibleRowCount(), 0u);
	f.h.typeInto(f.panel.nameFilterNode(), "BE*");
	EXPECT_EQ(f.panel.visibleRowCount(), 1u);
	f.h.typeInto(f.panel.nameFilterNode(), "");
	EXPECT_EQ(f.panel.visibleRowCount(), 3u);
}

TEST(FunctionListPanelUiTest, AddressConfidenceAndPatternFilters)
{
	PanelFixture f;
	f.h.typeInto(f.panel.addrLowNode(), "0x402000");
	EXPECT_EQ(f.panel.visibleRowCount(), 2u);
	f.h.typeInto(f.panel.addrHighNode(), "402fff");
	EXPECT_EQ(f.panel.visibleRowCount(), 1u);
	f.h.typeInto(f.panel.addrLowNode(), "");
	f.h.typeInto(f.panel.addrHighNode(), "");
	EXPECT_EQ(f.panel.visibleRowCount(), 3u);

	// Confidence spin box: arrow up four times = 0.20, then to 0.55.
	f.h.ui.setFocus(f.panel.confidenceNode(), fuse::ui::FocusReason::Keyboard);
	for (int i = 0; i < 11; ++i)
		f.h.key(fuse::ui::Key::Up);
	EXPECT_NEAR(f.panel.filter().minConfidence, 0.55f, 1e-4f);
	EXPECT_EQ(f.panel.visibleRowCount(), 1u);
	f.panel.setMinConfidence(0.f);
	EXPECT_EQ(f.panel.visibleRowCount(), 3u);

	// Pattern check boxes OR together.
	f.h.clickNode(f.panel.patternCheckNode(1)); // Crypto
	EXPECT_EQ(f.panel.visibleRowCount(), 1u);
	f.h.clickNode(f.panel.patternCheckNode(4)); // Lib
	EXPECT_EQ(f.panel.visibleRowCount(), 2u);
	f.panel.setPatternFilter(false, false, false, false, false);
	EXPECT_EQ(f.panel.visibleRowCount(), 3u);
}

TEST(FunctionListPanelUiTest, ClickingHeadersSorts)
{
	PanelFixture f;
	f.h.click(f.panel.headerCenter(FnColConfidence));
	EXPECT_EQ(f.panel.sortColumn(), FnColConfidence);
	EXPECT_TRUE(f.panel.sortAscending());
	EXPECT_EQ(f.panel.cellText(0, FnColName), "gamma");
	f.h.click(f.panel.headerCenter(FnColConfidence));
	EXPECT_FALSE(f.panel.sortAscending());
	EXPECT_EQ(f.panel.cellText(0, FnColName), "alpha");

	f.h.click(f.panel.headerCenter(FnColName));
	EXPECT_EQ(f.panel.cellText(0, FnColName), "alpha");
	EXPECT_EQ(f.panel.cellText(2, FnColName), "gamma");
	f.h.click(f.panel.headerCenter(FnColName));
	EXPECT_EQ(f.panel.cellText(0, FnColName), "gamma");
}

TEST(FunctionListPanelUiTest, ClickSelectsAndDoubleClickActivates)
{
	PanelFixture f;
	int selectionChanges = 0;
	std::vector<std::pair<uint64_t, std::string>> activated;
	f.panel.onSelectionChanged = [&] { ++selectionChanges; };
	f.panel.onFunctionActivated = [&](uint64_t a, const std::string& n) { activated.emplace_back(a, n); };

	f.h.click(f.panel.cellCenter(1, FnColAddress));
	EXPECT_GE(selectionChanges, 1);
	ASSERT_TRUE(f.panel.selectedEntry().has_value());
	EXPECT_EQ(f.panel.selectedEntry()->address, 0x402000u);
	EXPECT_EQ(f.h.ui.value(f.panel.tableNode()), 1.f);
	EXPECT_TRUE(activated.empty());

	f.h.click(f.panel.cellCenter(1, FnColAddress));
	ASSERT_EQ(activated.size(), 1u);
	EXPECT_EQ(activated[0].first, 0x402000u);
	EXPECT_EQ(activated[0].second, "beta");

	// Enter on the focused table activates the current row too.
	f.h.key(fuse::ui::Key::Down);
	EXPECT_EQ(f.panel.selectedEntry()->address, 0x403000u);
	f.h.key(fuse::ui::Key::Enter);
	ASSERT_EQ(activated.size(), 2u);
	EXPECT_EQ(activated[1].first, 0x403000u);
}

TEST(FunctionListPanelUiTest, CtrlAndShiftClickMultiSelect)
{
	PanelFixture f;
	f.h.click(f.panel.cellCenter(0, FnColAddress));
	f.h.click(f.panel.cellCenter(2, FnColAddress), fuse::ui::MouseButton::Left, fuse::ui::kModCtrl);
	EXPECT_EQ(f.panel.selectedAddresses(), (std::vector<uint64_t>{0x401000, 0x403000}));
	f.h.click(f.panel.cellCenter(0, FnColAddress), fuse::ui::MouseButton::Left, fuse::ui::kModCtrl);
	EXPECT_EQ(f.panel.selectedAddresses(), (std::vector<uint64_t>{0x403000}));
	f.h.click(f.panel.cellCenter(2, FnColAddress), fuse::ui::MouseButton::Left, fuse::ui::kModShift);
	EXPECT_EQ(f.panel.selectedAddresses().size(), 3u); // anchor row 0 .. row 2

	// Shift+Up extends from the anchor; Ctrl+A selects everything.
	f.h.click(f.panel.cellCenter(2, FnColAddress));
	f.h.key(fuse::ui::Key::Up, fuse::ui::kModShift);
	EXPECT_EQ(f.panel.selectedAddresses(), (std::vector<uint64_t>{0x402000, 0x403000}));
	f.h.key(fuse::ui::Key::Up);
	EXPECT_EQ(f.panel.selectedAddresses(), (std::vector<uint64_t>{0x401000}));
	f.h.key(fuse::ui::Key::A, fuse::ui::kModCtrl);
	EXPECT_EQ(f.panel.selectedAddresses().size(), 3u);

	// Filtering drops hidden rows from the selection.
	f.panel.setNameFilterText("beta");
	EXPECT_EQ(f.panel.selectedAddresses(), (std::vector<uint64_t>{0x402000}));
}

TEST(FunctionListPanelUiTest, ContextMenuGoToAndRedecompile)
{
	PanelFixture f;
	std::vector<uint64_t> navigated;
	std::vector<std::pair<uint64_t, std::string>> redecompile;
	f.panel.onAddressActivated = [&](uint64_t a) { navigated.push_back(a); };
	f.panel.onRedecompileRequested = [&](uint64_t a, const std::string& n) { redecompile.emplace_back(a, n); };

	f.h.click(f.panel.cellCenter(0, FnColName), fuse::ui::MouseButton::Right);
	ASSERT_TRUE(w::popupOpen(f.h.ui, f.panel.contextMenu()));
	const auto& labels = f.panel.contextMenuLabels();
	ASSERT_FALSE(labels.empty());
	EXPECT_EQ(labels.front(), "Go to Function");
	EXPECT_NE(std::find(labels.begin(), labels.end(), "Copy Signature"), labels.end());
	EXPECT_NE(std::find(labels.begin(), labels.end(), "Export JSON"), labels.end());
	EXPECT_EQ(f.panel.selectedEntry()->address, 0x401000u); // right-click selects

	f.h.clickNode(f.h.findByText("Go to Function"));
	EXPECT_EQ(navigated, (std::vector<uint64_t>{0x401000}));
	f.h.frame();
	EXPECT_FALSE(w::popupOpen(f.h.ui, f.panel.contextMenu()));

	f.h.click(f.panel.cellCenter(1, FnColName), fuse::ui::MouseButton::Right);
	f.h.clickNode(f.h.findByText("Re-decompile Selected Function"));
	ASSERT_EQ(redecompile.size(), 1u);
	EXPECT_EQ(redecompile[0].first, 0x402000u);
	EXPECT_EQ(redecompile[0].second, "beta");

	// Right-click below the rows: only the export entries.
	f.h.click(
		{f.h.ui.contentRect(f.panel.tableNode()).x + 20.f, f.h.ui.contentRect(f.panel.tableNode()).bottom() - 5.f},
		fuse::ui::MouseButton::Right);
	EXPECT_EQ(f.panel.contextMenuLabels().front(), "Add Tag to Selected\xE2\x80\xA6");
	f.h.key(fuse::ui::Key::Escape);
}

TEST(FunctionListPanelUiTest, RenameNotesSignatureTagAndClear)
{
	PanelFixture f;
	std::vector<std::string> renamed;
	int annotations = 0;
	f.panel.onFunctionRenamed = [&](uint64_t, const std::string&, const std::string& n) { renamed.push_back(n); };
	f.panel.onAnnotationChanged = [&](const FunctionEntry&) { ++annotations; };

	// F2 renames the current row; type into the dialog's field and press Enter.
	f.h.click(f.panel.cellCenter(0, FnColAddress));
	f.h.key(fuse::ui::Key::F2);
	ASSERT_TRUE(f.panel.promptOpen());
	EXPECT_EQ(f.h.ui.text(f.panel.promptEdit()), "alpha");
	f.h.ui.textInput("alpha_renamed"); // replaces the selected text
	f.h.key(fuse::ui::Key::Enter);
	EXPECT_FALSE(f.panel.promptOpen());
	EXPECT_EQ(renamed, (std::vector<std::string>{"alpha_renamed"}));
	EXPECT_EQ(f.panel.cellText(0, FnColName), "alpha_renamed");

	// Double-click on the Notes cell edits the notes.
	f.h.click(f.panel.cellCenter(0, FnColNotes));
	f.h.click(f.panel.cellCenter(0, FnColNotes));
	ASSERT_TRUE(f.panel.promptOpen());
	f.panel.acceptPrompt("entry point");
	EXPECT_EQ(f.panel.cellText(0, FnColNotes), "entry point");

	f.panel.triggerAction(FunctionListPanel::Action::EditSignature, 0x401000);
	f.panel.acceptPrompt("  void alpha(void)  ");
	EXPECT_EQ(f.panel.selectedEntry()->signature, "void alpha(void)");

	// Tag every selected function.
	f.h.frame(); // let the closed dialog's scrim go before clicking the table
	f.h.click(f.panel.cellCenter(1, FnColAddress), fuse::ui::MouseButton::Left, fuse::ui::kModCtrl);
	f.panel.triggerAction(FunctionListPanel::Action::AddTag);
	f.panel.acceptPrompt("hot");
	EXPECT_EQ(f.panel.cellText(0, FnColNotes), "entry point  [hot]");
	EXPECT_EQ(f.panel.cellText(1, FnColNotes), "check me  [net, hot]");

	// Cancel leaves things alone.
	f.panel.triggerAction(FunctionListPanel::Action::Rename, 0x403000);
	f.panel.cancelPrompt();
	EXPECT_EQ(f.panel.cellText(2, FnColName), "gamma");

	// Clear annotation restores the raw name and drops notes / tags / signature.
	f.panel.triggerAction(FunctionListPanel::Action::ClearAnnotation, 0x401000);
	EXPECT_EQ(f.panel.cellText(0, FnColName), "_Zalpha");
	EXPECT_EQ(f.panel.cellText(0, FnColNotes), "");
	EXPECT_GE(annotations, 4);
	EXPECT_EQ(renamed.back(), "_Zalpha");
}

TEST(FunctionListPanelUiTest, CopyAndExport)
{
	PanelFixture f;
	f.h.clickNode(f.panel.copyButtonNode());
	std::string clip;
	ASSERT_TRUE(f.h.ui.clipboardText(clip));
	EXPECT_EQ(clip, f.panel.csvText());
	EXPECT_EQ(f.panel.statusText(), "3 functions");
	EXPECT_EQ(f.h.ui.text(f.panel.statusNode()), "Copied 3 functions");
	EXPECT_EQ(
		clip.substr(0, clip.find('\n')),
		"Address,Name,SizeBytes,Instrs,Confidence,CC,STL,Crypto,Algo,Design,Lib,Notes,Tags");

	// Ctrl+C on the table copies the current address.
	f.h.click(f.panel.cellCenter(1, FnColAddress));
	f.h.key(fuse::ui::Key::C, fuse::ui::kModCtrl);
	ASSERT_TRUE(f.h.ui.clipboardText(clip));
	EXPECT_EQ(clip, "0x402000");
	std::string written;
	f.panel.clipboardWriter = [&](const std::string& s) {
		written = s;
		return true;
	};
	f.panel.triggerAction(FunctionListPanel::Action::CopySignature);
	EXPECT_EQ(written, "int beta(char*)");

	const auto dir = std::filesystem::temp_directory_path() / "retdec-gui-funclist-test";
	std::filesystem::remove_all(dir);
	std::filesystem::create_directories(dir);
	ASSERT_TRUE(f.panel.exportJson((dir / "direct.json").string()));
	EXPECT_EQ(readFile(dir / "direct.json"), f.panel.jsonText());

	// The CSV button opens a save dialog; accept it in our directory.
	f.h.clickNode(f.panel.csvButtonNode());
	const auto dlg = f.panel.fileDialogNode();
	ASSERT_TRUE(f.h.ui.alive(dlg));
	ASSERT_TRUE(w::fileDialogNavigate(f.h.ui, dlg, dir.string()));
	f.h.frame();
	f.h.clickNode(f.h.findByText("Save"));
	EXPECT_EQ(readFile(dir / "functions.csv"), f.panel.csvText());
	EXPECT_NE(std::string(f.h.ui.text(f.panel.statusNode())).find("Exported 3 functions"), std::string::npos);
	std::filesystem::remove_all(dir);
}

TEST(FunctionListPanelUiTest, DemangleToggleAndShiftF10Menu)
{
	PanelFixture f;
	f.panel.setDemangleNames(false);
	EXPECT_EQ(f.panel.cellText(0, FnColName), "_Zalpha");
	f.panel.setDemangleNames(true);
	f.h.click(f.panel.cellCenter(2, FnColAddress));
	f.h.key(fuse::ui::Key::F10, fuse::ui::kModShift);
	ASSERT_TRUE(w::popupOpen(f.h.ui, f.panel.contextMenu()));
	EXPECT_EQ(f.panel.contextMenuLabels().front(), "Go to Function");
	f.h.key(fuse::ui::Key::Escape);
	f.h.frame();
	EXPECT_FALSE(w::popupOpen(f.h.ui, f.panel.contextMenu()));
}

TEST(FunctionListPanelUiTest, HoverShowsCellToolTip)
{
	PanelFixture f;
	const auto p = f.panel.cellCenter(1, FnColConfidence);
	f.h.ui.pointerMove(p);
	f.h.frame();
	EXPECT_FALSE(f.h.ui.alive(f.panel.cellToolTipNode()));
	f.h.ui.update(0.6f);
	f.h.frame();
	ASSERT_TRUE(f.h.ui.alive(f.panel.cellToolTipNode()));
	EXPECT_TRUE(f.h.ui.computed(f.panel.cellToolTipNode()).visible);
	EXPECT_NE(std::string(f.h.ui.text(f.panel.cellToolTipNode())).find("Composite: 50%"), std::string::npos);
	// Another cell: the name, raw name and signature.
	f.h.ui.pointerMove(f.panel.cellCenter(1, FnColName));
	f.h.frame();
	EXPECT_FALSE(f.h.ui.computed(f.panel.cellToolTipNode()).visible);
	f.h.ui.update(0.6f);
	f.h.frame();
	EXPECT_EQ(f.h.ui.text(f.panel.cellToolTipNode()), "beta\n_Zbeta\nint beta(char*)");
	// Leaving the table hides it.
	const auto st = f.h.ui.rect(f.panel.statusNode());
	f.h.ui.pointerMove({st.x + 4.f, st.y + st.h * 0.5f});
	f.h.frame();
	EXPECT_FALSE(f.h.ui.computed(f.panel.cellToolTipNode()).visible);
}

TEST(FunctionListPanelUiTest, RendersToPng)
{
	Headless h({1100.f, 360.f});
	FunctionListPanel panel(h.ui, h.ui.root());
	std::vector<FunctionEntry> fns;
	for (int i = 0; i < 14; ++i)
	{
		auto e =
			makeFn(0x401000 + 0x40 * i, "function_" + std::to_string(i), 0.1f + 0.065f * i, i % 3 == 0, i % 4 == 1);
		e.patterns.algo = i % 5 == 2;
		e.patterns.design = i % 6 == 3;
		e.isLibrary = e.patterns.library = i % 7 == 6;
		e.cc = i % 2 ? "stdcall" : "cdecl";
		if (i == 2) e.notes = "decrypts the config";
		if (i == 4) e.tags = {"net", "c2"};
		fns.push_back(e);
	}
	panel.setFunctions(std::move(fns));
	h.frame();
	h.click(panel.cellCenter(2, FnColAddress));
	h.click(panel.cellCenter(4, FnColAddress), fuse::ui::MouseButton::Left, fuse::ui::kModCtrl);
	h.ui.pointerMove(panel.cellCenter(7, FnColConfidence));
	h.ui.update(0.6f);
	EXPECT_GT(h.render("function_list_panel.png"), 1000u);

	Headless e({700.f, 240.f});
	FunctionListPanel empty(e.ui, e.ui.root());
	EXPECT_GT(e.render("function_list_panel_empty.png"), 100u);
}
