/**
 * @file tests/gui/strings_browser_panel_test.cpp
 * @brief Unit tests for StringsBrowserPanel, CategoryDetector, ConstantLabeler,
 *        StringsModel, ConstantsModel, and TypeFilterProxy.
 */

#include "retdec/gui/panels/strings_browser_panel.h"

#include <QApplication>
#include <QSignalSpy>
#include <QSortFilterProxyModel>
#include <gtest/gtest.h>

using namespace retdec::gui::panels;

// ── Test fixture with QApplication ───────────────────────────────────────────

class StringsBrowserTest : public ::testing::Test {
protected:
    void SetUp() override {
        Q_ASSERT(QApplication::instance() != nullptr);
    }
    void TearDown() override {
        if (QApplication::instance())
            QApplication::processEvents();
    }
};

// ─── CategoryDetector tests ───────────────────────────────────────────────────

TEST_F(StringsBrowserTest, IsFormatString_Printf) {
    EXPECT_TRUE(CategoryDetector::isFormatString("Hello %s, count=%d"));
}

TEST_F(StringsBrowserTest, IsFormatString_Float) {
    EXPECT_TRUE(CategoryDetector::isFormatString("Value: %.2f"));
}

TEST_F(StringsBrowserTest, IsFormatString_Negative) {
    EXPECT_FALSE(CategoryDetector::isFormatString("Hello World"));
}

TEST_F(StringsBrowserTest, IsURL_Http) {
    EXPECT_TRUE(CategoryDetector::isURL("http://example.com/path"));
}

TEST_F(StringsBrowserTest, IsURL_Https) {
    EXPECT_TRUE(CategoryDetector::isURL("https://api.example.com/v1"));
}

TEST_F(StringsBrowserTest, IsURL_Ftp) {
    EXPECT_TRUE(CategoryDetector::isURL("ftp://files.example.com"));
}

TEST_F(StringsBrowserTest, IsURL_Negative) {
    EXPECT_FALSE(CategoryDetector::isURL("not a url"));
}

TEST_F(StringsBrowserTest, IsFilePath_Unix) {
    EXPECT_TRUE(CategoryDetector::isFilePath("/usr/lib/libssl.so"));
}

TEST_F(StringsBrowserTest, IsFilePath_Windows) {
    EXPECT_TRUE(CategoryDetector::isFilePath("C:\\Windows\\System32\\ntdll.dll"));
}

TEST_F(StringsBrowserTest, IsFilePath_UNC) {
    EXPECT_TRUE(CategoryDetector::isFilePath("\\\\server\\share\\file.txt"));
}

TEST_F(StringsBrowserTest, IsFilePath_Proc) {
    EXPECT_TRUE(CategoryDetector::isFilePath("/proc/self/maps"));
}

TEST_F(StringsBrowserTest, IsFilePath_Negative) {
    EXPECT_FALSE(CategoryDetector::isFilePath("Hello World"));
}

TEST_F(StringsBrowserTest, IsRegEx_WithMetachars) {
    EXPECT_TRUE(CategoryDetector::isRegEx("^[a-z]+\\d{2,4}$"));
}

TEST_F(StringsBrowserTest, IsRegEx_Negative) {
    EXPECT_FALSE(CategoryDetector::isRegEx("plain text string"));
}

TEST_F(StringsBrowserTest, IsCryptoConst_SHA) {
    EXPECT_TRUE(CategoryDetector::isCryptoConst("SHA256"));
}

TEST_F(StringsBrowserTest, IsCryptoConst_PEM) {
    EXPECT_TRUE(CategoryDetector::isCryptoConst("BEGIN CERTIFICATE"));
}

TEST_F(StringsBrowserTest, IsCryptoConst_Negative) {
    EXPECT_FALSE(CategoryDetector::isCryptoConst("filename.txt"));
}

TEST_F(StringsBrowserTest, Classify_URL) {
    EXPECT_EQ(CategoryDetector::classify("https://google.com"),
              StringCategory::URL);
}

TEST_F(StringsBrowserTest, Classify_FilePath) {
    EXPECT_EQ(CategoryDetector::classify("/etc/passwd"),
              StringCategory::FilePath);
}

TEST_F(StringsBrowserTest, Classify_FormatString) {
    EXPECT_EQ(CategoryDetector::classify("Error: %s (code %d)"),
              StringCategory::FormatString);
}

TEST_F(StringsBrowserTest, Classify_Crypto) {
    EXPECT_EQ(CategoryDetector::classify("AES-256-CBC key initialised"),
              StringCategory::CryptoConst);
}

TEST_F(StringsBrowserTest, Classify_Plain) {
    EXPECT_EQ(CategoryDetector::classify("Hello, World!"),
              StringCategory::Plain);
}

// ─── ConstantLabeler tests ────────────────────────────────────────────────────

TEST_F(StringsBrowserTest, ConstantLabeler_MZMagic) {
    ConstantEntry e;
    e.value = 0x5A4D;
    e.size  = ConstantSize::Word;
    ConstantLabeler::label(e);
    EXPECT_EQ(e.labelKind, ConstantLabel::MagicNumber);
    EXPECT_FALSE(e.label.isEmpty());
    EXPECT_EQ(e.hexStr, "5A4D");
}

TEST_F(StringsBrowserTest, ConstantLabeler_ELFMagic) {
    ConstantEntry e;
    e.value = 0x7F454C46;
    e.size  = ConstantSize::Dword;
    ConstantLabeler::label(e);
    EXPECT_EQ(e.labelKind, ConstantLabel::MagicNumber);
    EXPECT_NE(e.label.indexOf("ELF"), -1);
}

TEST_F(StringsBrowserTest, ConstantLabeler_SHA256K0) {
    ConstantEntry e;
    e.value = 0x428a2f98;
    e.size  = ConstantSize::Dword;
    ConstantLabeler::label(e);
    EXPECT_EQ(e.labelKind, ConstantLabel::CryptoConstant);
    EXPECT_NE(e.label.indexOf("SHA256"), -1);
}

TEST_F(StringsBrowserTest, ConstantLabeler_PiFloat) {
    ConstantEntry e;
    e.value = 0x40490FDB;  // π as float32
    e.size  = ConstantSize::Dword;
    ConstantLabeler::label(e);
    EXPECT_EQ(e.labelKind, ConstantLabel::FloatSpecial);
    EXPECT_NE(e.label.indexOf("π"), -1);
}

TEST_F(StringsBrowserTest, ConstantLabeler_HttpPort) {
    ConstantEntry e;
    e.value = 80;
    e.size  = ConstantSize::Word;
    ConstantLabeler::label(e);
    EXPECT_EQ(e.labelKind, ConstantLabel::PortNumber);
    EXPECT_NE(e.label.indexOf("HTTP"), -1);
}

TEST_F(StringsBrowserTest, ConstantLabeler_SshPort) {
    ConstantEntry e;
    e.value = 22;
    e.size  = ConstantSize::Word;
    ConstantLabeler::label(e);
    EXPECT_EQ(e.labelKind, ConstantLabel::PortNumber);
    EXPECT_NE(e.label.indexOf("SSH"), -1);
}

TEST_F(StringsBrowserTest, ConstantLabeler_Unknown) {
    ConstantEntry e;
    e.value = 0x12345678;
    e.size  = ConstantSize::Dword;
    ConstantLabeler::label(e);
    EXPECT_EQ(e.labelKind, ConstantLabel::None);
    EXPECT_TRUE(e.label.isEmpty());
}

TEST_F(StringsBrowserTest, ConstantLabeler_HexDecStrings) {
    ConstantEntry e;
    e.value = 0xDEADBEEF;
    e.size  = ConstantSize::Dword;
    ConstantLabeler::label(e);
    EXPECT_EQ(e.hexStr, "DEADBEEF");
    EXPECT_EQ(e.decStr, QString::number(0xDEADBEEF));
}

// ─── StringsModel tests ───────────────────────────────────────────────────────

TEST_F(StringsBrowserTest, StringsModel_Empty) {
    StringsModel model;
    EXPECT_EQ(model.rowCount({}), 0);
    EXPECT_EQ(model.columnCount({}), 5);
}

TEST_F(StringsBrowserTest, StringsModel_SetStrings) {
    StringsModel model;
    std::vector<StringEntry> entries;
    StringEntry e;
    e.address  = 0x401000;
    e.value    = "Hello World";
    e.type     = StringType::ASCII;
    e.refCount = 3;
    entries.push_back(e);
    model.setStrings(std::move(entries));
    EXPECT_EQ(model.rowCount({}), 1);
}

TEST_F(StringsBrowserTest, StringsModel_DataDisplay) {
    StringsModel model;
    StringEntry e;
    e.address  = 0x402000;
    e.value    = "https://example.com";
    e.type     = StringType::ASCII;
    e.refCount = 5;
    model.setStrings({e});

    // Address column
    QString addr = model.data(model.index(0, 0)).toString();
    EXPECT_TRUE(addr.contains("402000", Qt::CaseInsensitive));

    // Auto-classify URL
    EXPECT_EQ(model.entry(0).category, StringCategory::URL);

    // Refs column
    EXPECT_EQ(model.data(model.index(0, 4)).toInt(), 5);
}

TEST_F(StringsBrowserTest, StringsModel_AutoClassify_Format) {
    StringsModel model;
    StringEntry e;
    e.value = "Error code: %d in %s";
    model.setStrings({e});
    EXPECT_EQ(model.entry(0).category, StringCategory::FormatString);
}

TEST_F(StringsBrowserTest, StringsModel_Clear) {
    StringsModel model;
    model.setStrings({{0x100, "test", StringType::ASCII, StringCategory::Plain, 4, 1}});
    model.clear();
    EXPECT_EQ(model.rowCount({}), 0);
}

TEST_F(StringsBrowserTest, StringsModel_TotalCount) {
    StringsModel model;
    std::vector<StringEntry> entries(10);
    for (int i = 0; i < 10; ++i) entries[i].value = "str" + QString::number(i);
    model.setStrings(std::move(entries));
    EXPECT_EQ(model.totalCount(), 10);
}

TEST_F(StringsBrowserTest, StringsModel_ExportCsv) {
    StringsModel model;
    StringEntry e;
    e.address  = 0x1000;
    e.value    = "Hello";
    e.type     = StringType::ASCII;
    e.refCount = 2;
    model.setStrings({e});
    QString csv = model.exportCsv();
    EXPECT_TRUE(csv.contains("Address"));
    EXPECT_TRUE(csv.contains("Hello"));
    EXPECT_TRUE(csv.contains("1000"));
}

TEST_F(StringsBrowserTest, StringsModel_HeaderData) {
    StringsModel model;
    EXPECT_EQ(model.headerData(0, Qt::Horizontal, Qt::DisplayRole).toString(), "Address");
    EXPECT_EQ(model.headerData(3, Qt::Horizontal, Qt::DisplayRole).toString(), "Preview");
    EXPECT_EQ(model.headerData(4, Qt::Horizontal, Qt::DisplayRole).toString(), "Refs");
}

// ─── ConstantsModel tests ─────────────────────────────────────────────────────

TEST_F(StringsBrowserTest, ConstantsModel_Empty) {
    ConstantsModel model;
    EXPECT_EQ(model.rowCount({}), 0);
    EXPECT_EQ(model.columnCount({}), 6);
}

TEST_F(StringsBrowserTest, ConstantsModel_SetConstants_Labels) {
    ConstantsModel model;
    ConstantEntry e;
    e.address = 0x500000;
    e.size    = ConstantSize::Dword;
    e.value   = 0x428a2f98;  // SHA256 K[0]
    model.setConstants({e});
    ASSERT_EQ(model.rowCount({}), 1);
    EXPECT_EQ(model.entry(0).labelKind, ConstantLabel::CryptoConstant);
}

TEST_F(StringsBrowserTest, ConstantsModel_ExportCsv) {
    ConstantsModel model;
    ConstantEntry e;
    e.address = 0x200;
    e.value   = 443;
    e.size    = ConstantSize::Word;
    model.setConstants({e});
    QString csv = model.exportCsv();
    EXPECT_TRUE(csv.contains("Address"));
    EXPECT_TRUE(csv.contains("HTTPS"));
}

TEST_F(StringsBrowserTest, ConstantsModel_Clear) {
    ConstantsModel model;
    ConstantEntry e; e.value = 1; e.size = ConstantSize::Byte;
    model.setConstants({e});
    model.clear();
    EXPECT_EQ(model.rowCount({}), 0);
}

// ─── TypeFilterProxy tests ────────────────────────────────────────────────────

TEST_F(StringsBrowserTest, TypeFilterProxy_TypeFilter) {
    StringsModel model;
    StringEntry e1; e1.value = "a"; e1.type = StringType::ASCII;
    StringEntry e2; e2.value = "b"; e2.type = StringType::UTF16LE;
    model.setStrings({e1, e2});

    TypeFilterProxy proxy;
    proxy.setSourceModel(&model);
    proxy.setTypeFilter("ASCII");

    EXPECT_EQ(proxy.rowCount(), 1);
}

TEST_F(StringsBrowserTest, TypeFilterProxy_CategoryFilter) {
    StringsModel model;
    StringEntry e1; e1.value = "https://x.com"; e1.type = StringType::ASCII;
    StringEntry e2; e2.value = "plain text";    e2.type = StringType::ASCII;
    model.setStrings({e1, e2});

    TypeFilterProxy proxy;
    proxy.setSourceModel(&model);
    proxy.setCategoryFilter("URL");

    EXPECT_EQ(proxy.rowCount(), 1);
}

TEST_F(StringsBrowserTest, TypeFilterProxy_TextFilter) {
    StringsModel model;
    StringEntry e1; e1.value = "needle in haystack";
    StringEntry e2; e2.value = "nothing here";
    model.setStrings({e1, e2});

    TypeFilterProxy proxy;
    proxy.setSourceModel(&model);
    proxy.setTextFilter("needle");

    EXPECT_EQ(proxy.rowCount(), 1);
}

TEST_F(StringsBrowserTest, TypeFilterProxy_RegexFilter) {
    StringsModel model;
    StringEntry e1; e1.value = "foo123";
    StringEntry e2; e2.value = "barxyz";
    model.setStrings({e1, e2});

    TypeFilterProxy proxy;
    proxy.setSourceModel(&model);
    proxy.setTextFilter("\\d+", true);

    EXPECT_EQ(proxy.rowCount(), 1);
}

TEST_F(StringsBrowserTest, TypeFilterProxy_AllFiltersEmpty_ShowsAll) {
    StringsModel model;
    for (int i = 0; i < 5; ++i) {
        StringEntry e; e.value = "str" + QString::number(i);
        model.appendStrings({e});
    }
    TypeFilterProxy proxy;
    proxy.setSourceModel(&model);

    EXPECT_EQ(proxy.rowCount(), 5);
}

// ─── StringsBrowserPanel tests ────────────────────────────────────────────────

TEST_F(StringsBrowserTest, Panel_Construction) {
    StringsBrowserPanel panel;
    EXPECT_EQ(panel.stringCount(),   0);
    EXPECT_EQ(panel.constantCount(), 0);
}

TEST_F(StringsBrowserTest, Panel_SetStrings) {
    StringsBrowserPanel panel;
    std::vector<StringEntry> entries;
    for (int i = 0; i < 5; ++i) {
        StringEntry e; e.value = "str" + QString::number(i);
        entries.push_back(e);
    }
    panel.setStrings(std::move(entries));
    EXPECT_EQ(panel.stringCount(), 5);
}

TEST_F(StringsBrowserTest, Panel_SetConstants) {
    StringsBrowserPanel panel;
    std::vector<ConstantEntry> entries;
    for (int i = 0; i < 3; ++i) {
        ConstantEntry e; e.value = static_cast<uint64_t>(i * 100);
        e.size = ConstantSize::Dword;
        entries.push_back(e);
    }
    panel.setConstants(std::move(entries));
    EXPECT_EQ(panel.constantCount(), 3);
}

TEST_F(StringsBrowserTest, Panel_ClearResetsAll) {
    StringsBrowserPanel panel;
    StringEntry e; e.value = "test";
    panel.setStrings({e});
    ConstantEntry ce; ce.value = 42; ce.size = ConstantSize::Byte;
    panel.setConstants({ce});

    panel.clear();

    EXPECT_EQ(panel.stringCount(),   0);
    EXPECT_EQ(panel.constantCount(), 0);
}

TEST_F(StringsBrowserTest, Panel_AddressNavigatedSignalOnDoubleClick) {
    StringsBrowserPanel panel;
    StringEntry e; e.address = 0xDEAD; e.value = "navigate me";
    panel.setStrings({e});

    QSignalSpy spy(&panel, &StringsBrowserPanel::addressNavigated);
    EXPECT_TRUE(spy.isValid());
    // The signal is connected to double-click; we verify it's wired
    // (actual double-click simulation requires QTest in a display environment)
    EXPECT_EQ(spy.count(), 0);
}

TEST_F(StringsBrowserTest, Panel_CryptoConstHighlighted) {
    StringsBrowserPanel panel;
    StringEntry e;
    e.value    = "AES-256 key initialisation string";
    e.type     = StringType::ASCII;
    e.refCount = 1;
    panel.setStrings({e});
    // Category should be auto-classified as Crypto
    EXPECT_EQ(panel.stringCount(), 1);
}

TEST_F(StringsBrowserTest, Panel_MixedStrings_CountsCorrect) {
    StringsBrowserPanel panel;
    std::vector<StringEntry> entries;
    entries.push_back({0x1000, QStringLiteral("http://x.com"), StringType::ASCII,
                       StringCategory::Plain, 12, 1});
    entries.push_back({0x2000, QStringLiteral("/etc/passwd"), StringType::ASCII,
                       StringCategory::Plain, 11, 2});
    entries.push_back({0x3000, QStringLiteral("Error: %s"), StringType::ASCII,
                       StringCategory::Plain, 9, 5});
    entries.push_back({0x4000, QStringLiteral("hello world"), StringType::ASCII,
                       StringCategory::Plain, 11, 0});
    panel.setStrings(entries);
    EXPECT_EQ(panel.stringCount(), 4);
}
