/**
 * @file tests/gui/strings_browser_panel_test.cpp
 * @brief Strings & Constants: the Qt StringsBrowserPanel tests ported to the
 *        model (CategoryDetector, ConstantLabeler, StringsTable, ConstantsTable,
 *        StringsProxy) and the FuseUI panel, plus headless UI tests.
 * @copyright (c) 2026 Odin Loch trading as Imortek
 */

#include "retdec/gui/model/strings_classify.h"
#include "retdec/gui/model/strings_model.h"
#include "retdec/gui/ui/strings_browser_panel.h"

#include "table_panels_test_util.h"

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <sstream>

using namespace retdec::gui::model;
using retdec::gui::ui::StringsBrowserPanel;
using retdec_gui_tp_test::Headless;
using Tab = StringsBrowserPanel::Table;
namespace w = fuse::ui::widgets;

// ─── Ported: CategoryDetector ────────────────────────────────────────────────

TEST(StringsBrowserTest, IsFormatString_Printf) { EXPECT_TRUE(CategoryDetector::isFormatString("Hello %s, count=%d")); }
TEST(StringsBrowserTest, IsFormatString_Float) { EXPECT_TRUE(CategoryDetector::isFormatString("Value: %.2f")); }
TEST(StringsBrowserTest, IsFormatString_Negative) { EXPECT_FALSE(CategoryDetector::isFormatString("Hello World")); }
TEST(StringsBrowserTest, IsURL_Http) { EXPECT_TRUE(CategoryDetector::isURL("http://example.com/path")); }
TEST(StringsBrowserTest, IsURL_Https) { EXPECT_TRUE(CategoryDetector::isURL("https://api.example.com/v1")); }
TEST(StringsBrowserTest, IsURL_Ftp) { EXPECT_TRUE(CategoryDetector::isURL("ftp://files.example.com")); }
TEST(StringsBrowserTest, IsURL_Negative) { EXPECT_FALSE(CategoryDetector::isURL("not a url")); }
TEST(StringsBrowserTest, IsFilePath_Unix) { EXPECT_TRUE(CategoryDetector::isFilePath("/usr/lib/libssl.so")); }
TEST(StringsBrowserTest, IsFilePath_Windows) { EXPECT_TRUE(CategoryDetector::isFilePath("C:\\Windows\\System32\\ntdll.dll")); }
TEST(StringsBrowserTest, IsFilePath_UNC) { EXPECT_TRUE(CategoryDetector::isFilePath("\\\\server\\share\\file.txt")); }
TEST(StringsBrowserTest, IsFilePath_Proc) { EXPECT_TRUE(CategoryDetector::isFilePath("/proc/self/maps")); }
TEST(StringsBrowserTest, IsFilePath_Negative) { EXPECT_FALSE(CategoryDetector::isFilePath("Hello World")); }
TEST(StringsBrowserTest, IsRegEx_WithMetachars) { EXPECT_TRUE(CategoryDetector::isRegEx("^[a-z]+\\d{2,4}$")); }
TEST(StringsBrowserTest, IsRegEx_Negative) { EXPECT_FALSE(CategoryDetector::isRegEx("plain text string")); }
TEST(StringsBrowserTest, IsCryptoConst_SHA) { EXPECT_TRUE(CategoryDetector::isCryptoConst("SHA256")); }
TEST(StringsBrowserTest, IsCryptoConst_PEM) { EXPECT_TRUE(CategoryDetector::isCryptoConst("BEGIN CERTIFICATE")); }
TEST(StringsBrowserTest, IsCryptoConst_Negative) { EXPECT_FALSE(CategoryDetector::isCryptoConst("filename.txt")); }

TEST(StringsBrowserTest, Classify_URL) { EXPECT_EQ(CategoryDetector::classify("https://google.com"), StringCategory::URL); }
TEST(StringsBrowserTest, Classify_FilePath) { EXPECT_EQ(CategoryDetector::classify("/etc/passwd"), StringCategory::FilePath); }
TEST(StringsBrowserTest, Classify_FormatString)
{
	EXPECT_EQ(CategoryDetector::classify("Error: %s (code %d)"), StringCategory::FormatString);
}
TEST(StringsBrowserTest, Classify_Crypto)
{
	EXPECT_EQ(CategoryDetector::classify("AES-256-CBC key initialised"), StringCategory::CryptoConst);
}
TEST(StringsBrowserTest, Classify_Plain) { EXPECT_EQ(CategoryDetector::classify("Hello, World!"), StringCategory::Plain); }

// ─── Ported: ConstantLabeler ─────────────────────────────────────────────────

TEST(StringsBrowserTest, ConstantLabeler_MZMagic)
{
	ConstantEntry e;
	e.value = 0x5A4D;
	e.size = ConstantSize::Word;
	ConstantLabeler::label(e);
	EXPECT_EQ(e.labelKind, ConstantLabel::MagicNumber);
	EXPECT_FALSE(e.label.empty());
	EXPECT_EQ(e.hexStr, "5A4D");
}

TEST(StringsBrowserTest, ConstantLabeler_ELFMagic)
{
	ConstantEntry e;
	e.value = 0x7F454C46;
	e.size = ConstantSize::Dword;
	ConstantLabeler::label(e);
	EXPECT_EQ(e.labelKind, ConstantLabel::MagicNumber);
	EXPECT_NE(e.label.find("ELF"), std::string::npos);
}

TEST(StringsBrowserTest, ConstantLabeler_SHA256K0)
{
	ConstantEntry e;
	e.value = 0x428a2f98;
	e.size = ConstantSize::Dword;
	ConstantLabeler::label(e);
	EXPECT_EQ(e.labelKind, ConstantLabel::CryptoConstant);
	EXPECT_NE(e.label.find("SHA256"), std::string::npos);
}

TEST(StringsBrowserTest, ConstantLabeler_PiFloat)
{
	ConstantEntry e;
	e.value = 0x40490FDB;
	e.size = ConstantSize::Dword;
	ConstantLabeler::label(e);
	EXPECT_EQ(e.labelKind, ConstantLabel::FloatSpecial);
	EXPECT_NE(e.label.find("\xCF\x80"), std::string::npos); // π
}

TEST(StringsBrowserTest, ConstantLabeler_HttpPort)
{
	ConstantEntry e;
	e.value = 80;
	e.size = ConstantSize::Word;
	ConstantLabeler::label(e);
	EXPECT_EQ(e.labelKind, ConstantLabel::PortNumber);
	EXPECT_NE(e.label.find("HTTP"), std::string::npos);
}

TEST(StringsBrowserTest, ConstantLabeler_SshPort)
{
	ConstantEntry e;
	e.value = 22;
	e.size = ConstantSize::Word;
	ConstantLabeler::label(e);
	EXPECT_EQ(e.labelKind, ConstantLabel::PortNumber);
	EXPECT_NE(e.label.find("SSH"), std::string::npos);
}

TEST(StringsBrowserTest, ConstantLabeler_Unknown)
{
	ConstantEntry e;
	e.value = 0x12345678;
	e.size = ConstantSize::Dword;
	ConstantLabeler::label(e);
	EXPECT_EQ(e.labelKind, ConstantLabel::None);
	EXPECT_TRUE(e.label.empty());
}

TEST(StringsBrowserTest, ConstantLabeler_HexDecStrings)
{
	ConstantEntry e;
	e.value = 0xDEADBEEF;
	e.size = ConstantSize::Dword;
	ConstantLabeler::label(e);
	EXPECT_EQ(e.hexStr, "DEADBEEF");
	EXPECT_EQ(e.decStr, std::to_string(0xDEADBEEFull));
}

// ─── Ported: StringsModel -> StringsTable ────────────────────────────────────

TEST(StringsBrowserTest, StringsModel_Empty)
{
	StringsTable model;
	EXPECT_EQ(model.size(), 0u);
	EXPECT_EQ(StrColCount, 5);
}

TEST(StringsBrowserTest, StringsModel_SetStrings)
{
	StringsTable model;
	StringEntry e;
	e.address = 0x401000;
	e.value = "Hello World";
	e.refCount = 3;
	model.setStrings({e});
	EXPECT_EQ(model.size(), 1u);
}

TEST(StringsBrowserTest, StringsModel_DataDisplay)
{
	StringsTable model;
	StringEntry e;
	e.address = 0x402000;
	e.value = "https://example.com";
	e.refCount = 5;
	model.setStrings({e});
	EXPECT_NE(stringCellText(model.entry(0), StrColAddress).find("402000"), std::string::npos);
	EXPECT_EQ(model.entry(0).category, StringCategory::URL);
	EXPECT_EQ(stringCellText(model.entry(0), StrColRefs), "5");
}

TEST(StringsBrowserTest, StringsModel_AutoClassify_Format)
{
	StringsTable model;
	StringEntry e;
	e.value = "Error code: %d in %s";
	model.setStrings({e});
	EXPECT_EQ(model.entry(0).category, StringCategory::FormatString);
}

TEST(StringsBrowserTest, StringsModel_Clear)
{
	StringsTable model;
	model.setStrings({{0x100, "test", StringType::ASCII, StringCategory::Plain, 4, 1}});
	model.clear();
	EXPECT_EQ(model.size(), 0u);
}

TEST(StringsBrowserTest, StringsModel_TotalCount)
{
	StringsTable model;
	std::vector<StringEntry> entries(10);
	for (int i = 0; i < 10; ++i)
		entries[static_cast<std::size_t>(i)].value = "str" + std::to_string(i);
	model.setStrings(std::move(entries));
	EXPECT_EQ(model.size(), 10u);
}

TEST(StringsBrowserTest, StringsModel_ExportCsv)
{
	StringsTable model;
	StringEntry e;
	e.address = 0x1000;
	e.value = "Hello";
	e.refCount = 2;
	model.setStrings({e});
	const std::string csv = model.exportCsv();
	EXPECT_NE(csv.find("Address"), std::string::npos);
	EXPECT_NE(csv.find("Hello"), std::string::npos);
	EXPECT_NE(csv.find("1000"), std::string::npos);
	EXPECT_EQ(csv, "Address,Type,Category,Value,Refs\n0x0000000000001000,ASCII,Plain,\"Hello\",2\n");
}

TEST(StringsBrowserTest, StringsModel_HeaderData)
{
	EXPECT_EQ(stringsColumnHeader(0), "Address");
	EXPECT_EQ(stringsColumnHeader(3), "Preview");
	EXPECT_EQ(stringsColumnHeader(4), "Refs");
}

// ─── Ported: ConstantsModel -> ConstantsTable ────────────────────────────────

TEST(StringsBrowserTest, ConstantsModel_Empty)
{
	ConstantsTable model;
	EXPECT_EQ(model.size(), 0u);
	EXPECT_EQ(ConstColCount, 6);
}

TEST(StringsBrowserTest, ConstantsModel_SetConstants_Labels)
{
	ConstantsTable model;
	ConstantEntry e;
	e.address = 0x500000;
	e.size = ConstantSize::Dword;
	e.value = 0x428a2f98;
	model.setConstants({e});
	ASSERT_EQ(model.size(), 1u);
	EXPECT_EQ(model.entry(0).labelKind, ConstantLabel::CryptoConstant);
}

TEST(StringsBrowserTest, ConstantsModel_ExportCsv)
{
	ConstantsTable model;
	ConstantEntry e;
	e.address = 0x200;
	e.value = 443;
	e.size = ConstantSize::Word;
	model.setConstants({e});
	const std::string csv = model.exportCsv();
	EXPECT_NE(csv.find("Address"), std::string::npos);
	EXPECT_NE(csv.find("HTTPS"), std::string::npos);
	EXPECT_EQ(csv, "Address,Size,Hex,Decimal,Label,Refs\n0x0000000000000200,2,01BB,443,\"HTTPS\",0\n");
}

TEST(StringsBrowserTest, ConstantsModel_Clear)
{
	ConstantsTable model;
	ConstantEntry e;
	e.value = 1;
	e.size = ConstantSize::Byte;
	model.setConstants({e});
	model.clear();
	EXPECT_EQ(model.size(), 0u);
}

// ─── Ported: TypeFilterProxy -> StringsProxy ─────────────────────────────────

TEST(StringsBrowserTest, TypeFilterProxy_TypeFilter)
{
	StringsTable model;
	StringEntry e1;
	e1.value = "a";
	e1.type = StringType::ASCII;
	StringEntry e2;
	e2.value = "b";
	e2.type = StringType::UTF16LE;
	model.setStrings({e1, e2});
	StringsProxy proxy(&model);
	proxy.setTypeFilter("ASCII");
	EXPECT_EQ(proxy.rowCount(), 1u);
}

TEST(StringsBrowserTest, TypeFilterProxy_CategoryFilter)
{
	StringsTable model;
	StringEntry e1;
	e1.value = "https://x.com";
	StringEntry e2;
	e2.value = "plain text";
	model.setStrings({e1, e2});
	StringsProxy proxy(&model);
	proxy.setCategoryFilter("URL");
	EXPECT_EQ(proxy.rowCount(), 1u);
}

TEST(StringsBrowserTest, TypeFilterProxy_TextFilter)
{
	StringsTable model;
	StringEntry e1;
	e1.value = "needle in haystack";
	StringEntry e2;
	e2.value = "nothing here";
	model.setStrings({e1, e2});
	StringsProxy proxy(&model);
	proxy.setTextFilter("needle");
	EXPECT_EQ(proxy.rowCount(), 1u);
}

TEST(StringsBrowserTest, TypeFilterProxy_RegexFilter)
{
	StringsTable model;
	StringEntry e1;
	e1.value = "foo123";
	StringEntry e2;
	e2.value = "barxyz";
	model.setStrings({e1, e2});
	StringsProxy proxy(&model);
	proxy.setTextFilter("\\d+", true);
	EXPECT_EQ(proxy.rowCount(), 1u);
}

TEST(StringsBrowserTest, TypeFilterProxy_AllFiltersEmpty_ShowsAll)
{
	StringsTable model;
	for (int i = 0; i < 5; ++i)
	{
		StringEntry e;
		e.value = "str" + std::to_string(i);
		model.appendStrings({e});
	}
	StringsProxy proxy(&model);
	EXPECT_EQ(proxy.rowCount(), 5u);
}

// ─── Ported: StringsBrowserPanel ─────────────────────────────────────────────

TEST(StringsBrowserTest, Panel_Construction)
{
	Headless h;
	StringsBrowserPanel panel(h.ui, h.ui.root());
	EXPECT_EQ(panel.stringCount(), 0);
	EXPECT_EQ(panel.constantCount(), 0);
}

TEST(StringsBrowserTest, Panel_SetStrings)
{
	Headless h;
	StringsBrowserPanel panel(h.ui, h.ui.root());
	std::vector<StringEntry> entries;
	for (int i = 0; i < 5; ++i)
	{
		StringEntry e;
		e.value = "str" + std::to_string(i);
		entries.push_back(e);
	}
	panel.setStrings(std::move(entries));
	EXPECT_EQ(panel.stringCount(), 5);
}

TEST(StringsBrowserTest, Panel_SetConstants)
{
	Headless h;
	StringsBrowserPanel panel(h.ui, h.ui.root());
	std::vector<ConstantEntry> entries;
	for (int i = 0; i < 3; ++i)
	{
		ConstantEntry e;
		e.value = static_cast<uint64_t>(i * 100);
		e.size = ConstantSize::Dword;
		entries.push_back(e);
	}
	panel.setConstants(std::move(entries));
	EXPECT_EQ(panel.constantCount(), 3);
}

TEST(StringsBrowserTest, Panel_ClearResetsAll)
{
	Headless h;
	StringsBrowserPanel panel(h.ui, h.ui.root());
	StringEntry e;
	e.value = "test";
	panel.setStrings({e});
	ConstantEntry ce;
	ce.value = 42;
	ce.size = ConstantSize::Byte;
	panel.setConstants({ce});
	panel.setStringFilterText("zzz");
	panel.clear();
	EXPECT_EQ(panel.stringCount(), 0);
	EXPECT_EQ(panel.constantCount(), 0);
	EXPECT_EQ(panel.stringFilterText(), "");
}

TEST(StringsBrowserTest, Panel_AddressNavigatedSignalOnDoubleClick)
{
	Headless h;
	StringsBrowserPanel panel(h.ui, h.ui.root());
	StringEntry e;
	e.address = 0xDEAD;
	e.value = "navigate me";
	panel.setStrings({e});
	h.frame();
	std::vector<uint64_t> got;
	panel.onAddressActivated = [&](uint64_t a) { got.push_back(a); };
	EXPECT_TRUE(got.empty());
	// Unlike the Qt test, a headless context can double-click for real.
	h.click(panel.cellCenter(Tab::Strings, 0, StrColPreview));
	h.click(panel.cellCenter(Tab::Strings, 0, StrColPreview));
	EXPECT_EQ(got, (std::vector<uint64_t>{0xDEAD}));
}

TEST(StringsBrowserTest, Panel_CryptoConstHighlighted)
{
	Headless h;
	StringsBrowserPanel panel(h.ui, h.ui.root());
	StringEntry e;
	e.value = "AES-256 key initialisation string";
	e.refCount = 1;
	panel.setStrings({e});
	EXPECT_EQ(panel.stringCount(), 1);
	EXPECT_EQ(panel.strings().entry(0).category, StringCategory::CryptoConst);
	EXPECT_EQ(stringCategoryColor(StringCategory::CryptoConst), 0xe6a23cu);
}

TEST(StringsBrowserTest, Panel_MixedStrings_CountsCorrect)
{
	Headless h;
	StringsBrowserPanel panel(h.ui, h.ui.root());
	std::vector<StringEntry> entries;
	entries.push_back({0x1000, "http://x.com", StringType::ASCII, StringCategory::Plain, 12, 1});
	entries.push_back({0x2000, "/etc/passwd", StringType::ASCII, StringCategory::Plain, 11, 2});
	entries.push_back({0x3000, "Error: %s", StringType::ASCII, StringCategory::Plain, 9, 5});
	entries.push_back({0x4000, "hello world", StringType::ASCII, StringCategory::Plain, 11, 0});
	panel.setStrings(entries);
	EXPECT_EQ(panel.stringCount(), 4);
}

// ─── Headless UI ─────────────────────────────────────────────────────────────

namespace {

struct StringsFixture
{
	Headless h;
	StringsBrowserPanel panel{h.ui, h.ui.root()};
	StringsFixture()
	{
		panel.setStrings({
			{0x4000, "hello world", StringType::ASCII, StringCategory::Plain, 11, 0},
			{0x1000, "http://x.com/a", StringType::ASCII, StringCategory::Plain, 14, 3},
			{0x2000, "/etc/passwd", StringType::UTF16LE, StringCategory::Plain, 11, 2},
			{0x3000, "Error: %s (code %d)", StringType::UTF8, StringCategory::Plain, 19, 7},
			{0x5000, "SHA256 digest", StringType::ASCII, StringCategory::Plain, 13, 1},
		});
		std::vector<ConstantEntry> cs;
		for (auto [a, v, sz]: std::vector<std::tuple<uint64_t, uint64_t, ConstantSize>>{
				 {0x600000, 0x428a2f98, ConstantSize::Dword},
				 {0x600010, 0x5A4D, ConstantSize::Word},
				 {0x600020, 443, ConstantSize::Word},
				 {0x600030, 0x40490FDB, ConstantSize::Dword},
				 {0x600040, 0x12345678, ConstantSize::Dword},
			 })
		{
			ConstantEntry c;
			c.address = a;
			c.value = v;
			c.size = sz;
			c.refCount = static_cast<int>(a & 0xff);
			cs.push_back(c);
		}
		panel.setConstants(std::move(cs));
		h.frame();
	}
};

} // namespace

TEST(StringsBrowserUiTest, FilterBoxRegexAndCombos)
{
	StringsFixture f;
	EXPECT_EQ(f.panel.visibleRowCount(Tab::Strings), 5u);
	EXPECT_EQ(f.panel.statusText(Tab::Strings), "5 / 5 strings");

	f.h.typeInto(f.panel.searchNode(Tab::Strings), "PASS");
	EXPECT_EQ(f.panel.visibleRowCount(Tab::Strings), 1u);
	EXPECT_EQ(f.h.ui.text(f.panel.statusNode(Tab::Strings)), "1 / 5 strings");

	// "/regex/" syntax, then the ".*" button.
	f.h.typeInto(f.panel.searchNode(Tab::Strings), "/^[a-z]+ w/");
	ASSERT_EQ(f.panel.visibleRowCount(Tab::Strings), 1u);
	EXPECT_EQ(f.panel.cellText(Tab::Strings, 0, StrColPreview), "hello world");
	f.h.typeInto(f.panel.searchNode(Tab::Strings), "%[sd]");
	EXPECT_EQ(f.panel.visibleRowCount(Tab::Strings), 0u);
	f.h.clickNode(f.panel.regexButtonNode());
	EXPECT_TRUE(f.panel.regexEnabled());
	EXPECT_EQ(f.panel.visibleRowCount(Tab::Strings), 1u);
	f.h.typeInto(f.panel.searchNode(Tab::Strings), "([");
	EXPECT_EQ(f.panel.visibleRowCount(Tab::Strings), 0u); // invalid regex shows nothing
	f.h.clickNode(f.panel.regexButtonNode());
	f.h.typeInto(f.panel.searchNode(Tab::Strings), "");
	EXPECT_EQ(f.panel.visibleRowCount(Tab::Strings), 5u);

	// Type and category combos (keyboard: focus + Down picks the next item).
	f.h.ui.setFocus(f.panel.typeComboNode(), fuse::ui::FocusReason::Keyboard);
	f.h.key(fuse::ui::Key::Down); // ASCII
	EXPECT_EQ(f.panel.visibleRowCount(Tab::Strings), 3u);
	f.panel.setTypeFilterIndex(0);
	f.panel.setCategoryFilterIndex(4); // FilePath
	ASSERT_EQ(f.panel.visibleRowCount(Tab::Strings), 1u);
	EXPECT_EQ(f.panel.cellText(Tab::Strings, 0, StrColCategory), "FilePath");
	f.panel.setCategoryFilterIndex(6); // Crypto
	ASSERT_EQ(f.panel.visibleRowCount(Tab::Strings), 1u);
	EXPECT_EQ(f.panel.cellText(Tab::Strings, 0, StrColAddress), "0x0000000000005000");
	f.panel.setCategoryFilterIndex(0);
}

TEST(StringsBrowserUiTest, HeaderSortingBothTables)
{
	StringsFixture f;
	f.h.click(f.panel.headerCenter(Tab::Strings, StrColAddress));
	EXPECT_EQ(f.panel.cellText(Tab::Strings, 0, StrColAddress), "0x0000000000001000");
	f.h.click(f.panel.headerCenter(Tab::Strings, StrColAddress));
	EXPECT_EQ(f.panel.cellText(Tab::Strings, 0, StrColAddress), "0x0000000000005000");
	f.h.click(f.panel.headerCenter(Tab::Strings, StrColRefs));
	EXPECT_EQ(f.panel.cellText(Tab::Strings, 0, StrColRefs), "0");
	EXPECT_EQ(f.panel.cellText(Tab::Strings, 4, StrColRefs), "7");

	f.panel.showTab(1);
	f.h.frame();
	EXPECT_EQ(f.panel.currentTab(), 1);
	f.h.click(f.panel.headerCenter(Tab::Constants, ConstColDec));
	EXPECT_EQ(f.panel.cellText(Tab::Constants, 0, ConstColDec), "443");
	f.h.click(f.panel.headerCenter(Tab::Constants, ConstColLabel));
	f.h.click(f.panel.headerCenter(Tab::Constants, ConstColLabel));
	EXPECT_EQ(f.panel.cellText(Tab::Constants, 0, ConstColLabel), "\xCF\x80 (3.14159\xE2\x80\xA6)");
}

TEST(StringsBrowserUiTest, ConstantsFilterAndKinds)
{
	StringsFixture f;
	f.panel.showTab(1);
	f.h.frame();
	EXPECT_EQ(f.panel.statusText(Tab::Constants), "5 / 5 constants");
	f.h.typeInto(f.panel.searchNode(Tab::Constants), "https");
	EXPECT_EQ(f.panel.visibleRowCount(Tab::Constants), 1u);
	f.h.typeInto(f.panel.searchNode(Tab::Constants), "01bb"); // hex column, any case
	EXPECT_EQ(f.panel.visibleRowCount(Tab::Constants), 1u);
	// A label kind takes over; the text is ignored while it is set.
	f.panel.setConstantKindIndex(2); // Crypto
	ASSERT_EQ(f.panel.visibleRowCount(Tab::Constants), 1u);
	EXPECT_EQ(f.panel.cellText(Tab::Constants, 0, ConstColLabel), "SHA256_K[0]");
	f.panel.setConstantKindIndex(1); // Magic
	EXPECT_EQ(f.panel.cellText(Tab::Constants, 0, ConstColLabel), "MZ (PE)");
	f.panel.setConstantKindIndex(0);
	EXPECT_EQ(f.panel.visibleRowCount(Tab::Constants), 1u);
	f.h.typeInto(f.panel.searchNode(Tab::Constants), "");
	EXPECT_EQ(f.panel.visibleRowCount(Tab::Constants), 5u);
}

TEST(StringsBrowserUiTest, ActivateCopyAndContextMenu)
{
	StringsFixture f;
	std::vector<uint64_t> got;
	f.panel.onAddressActivated = [&](uint64_t a) { got.push_back(a); };
	f.h.click(f.panel.cellCenter(Tab::Strings, 1, StrColPreview));
	EXPECT_EQ(f.h.ui.value(f.panel.tableNode(Tab::Strings)), 1.f);
	f.h.key(fuse::ui::Key::Enter);
	EXPECT_EQ(got, (std::vector<uint64_t>{0x1000}));

	f.h.key(fuse::ui::Key::C, fuse::ui::kModCtrl);
	std::string clip;
	ASSERT_TRUE(f.h.ui.clipboardText(clip));
	EXPECT_EQ(clip, "0x0000000000001000");

	f.h.click(f.panel.cellCenter(Tab::Strings, 3, StrColPreview), fuse::ui::MouseButton::Right);
	ASSERT_TRUE(w::popupOpen(f.h.ui, f.panel.contextMenu()));
	EXPECT_EQ(f.panel.contextMenuLabels(), (std::vector<std::string>{"Copy Address", "Copy Value"}));
	f.h.clickNode(f.h.findByText("Copy Value"));
	ASSERT_TRUE(f.h.ui.clipboardText(clip));
	EXPECT_EQ(clip, "Error: %s (code %d)");

	f.panel.showTab(1);
	f.h.frame();
	f.h.click(f.panel.cellCenter(Tab::Constants, 2, ConstColHex));
	f.h.key(fuse::ui::Key::F10, fuse::ui::kModShift);
	f.h.clickNode(f.h.findByText("Copy Value"));
	ASSERT_TRUE(f.h.ui.clipboardText(clip));
	EXPECT_EQ(clip, "0x01BB");
	f.h.click(f.panel.cellCenter(Tab::Constants, 0, ConstColHex));
	f.h.click(f.panel.cellCenter(Tab::Constants, 0, ConstColHex));
	EXPECT_EQ(got.back(), 0x600000u);
}

TEST(StringsBrowserUiTest, ExportThroughDialog)
{
	StringsFixture f;
	const auto dir = std::filesystem::temp_directory_path() / "retdec-gui-strings-test";
	std::filesystem::remove_all(dir);
	std::filesystem::create_directories(dir);
	f.h.clickNode(f.panel.exportButtonNode(Tab::Strings));
	ASSERT_TRUE(w::fileDialogNavigate(f.h.ui, f.panel.fileDialogNode(), dir.string()));
	f.h.frame();
	f.h.clickNode(f.h.findByText("Save"));
	std::ifstream in(dir / "strings.csv");
	std::stringstream ss;
	ss << in.rdbuf();
	EXPECT_EQ(ss.str(), f.panel.stringsCsv());
	ASSERT_TRUE(f.panel.exportConstantsCsv((dir / "c.csv").string()));
	EXPECT_TRUE(std::filesystem::exists(dir / "c.csv"));
	std::filesystem::remove_all(dir);
}

TEST(StringsBrowserUiTest, HoverShowsFullValueAndLabel)
{
	StringsFixture f;
	f.h.ui.pointerMove(f.panel.cellCenter(Tab::Strings, 3, StrColPreview));
	f.h.ui.update(0.6f);
	f.h.frame();
	ASSERT_TRUE(f.h.ui.alive(f.panel.cellToolTipNode()));
	EXPECT_EQ(f.h.ui.text(f.panel.cellToolTipNode()), "Error: %s (code %d)");
	f.panel.showTab(1);
	f.h.frame();
	f.h.ui.pointerMove(f.panel.cellCenter(Tab::Constants, 2, ConstColLabel));
	f.h.ui.update(0.6f);
	f.h.frame();
	EXPECT_EQ(f.h.ui.text(f.panel.cellToolTipNode()), "HTTPS (0x1bb)");
	EXPECT_TRUE(f.h.ui.computed(f.panel.cellToolTipNode()).visible);
}

TEST(StringsBrowserUiTest, RendersToPng)
{
	StringsFixture f;
	f.h.click(f.panel.cellCenter(Tab::Strings, 2, StrColPreview));
	EXPECT_GT(f.h.render("strings_browser_panel.png"), 1000u);
	f.panel.showTab(1);
	f.h.frame();
	EXPECT_GT(f.h.render("strings_browser_panel_constants.png"), 1000u);
}
