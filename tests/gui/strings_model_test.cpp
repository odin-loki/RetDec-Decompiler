/**
 * @file tests/gui/strings_model_test.cpp
 * @brief Strings & constants model: preview truncation, search syntax, filters
 *        and sorting beyond the ported Qt tests.
 * @copyright (c) 2026 Odin Loch trading as Imortek
 */

#include "retdec/gui/model/strings_model.h"

#include <gtest/gtest.h>

using namespace retdec::gui::model;

TEST(StringsModelTest, PreviewIsEightyCodePoints)
{
	StringEntry e;
	e.value = std::string(100, 'a');
	EXPECT_EQ(stringCellText(e, StrColPreview).size(), 80u);
	EXPECT_EQ(stringToolTip(e, StrColPreview), e.value);
	EXPECT_TRUE(stringToolTip(e, StrColAddress).empty());
	std::string pis;
	for (int i = 0; i < 90; ++i)
		pis += "\xCF\x80";
	EXPECT_EQ(utf8Left(pis, 80).size(), 160u); // never splits a code point
	EXPECT_EQ(utf8Left("ab", 80), "ab");
}

TEST(StringsModelTest, SearchSyntax)
{
	EXPECT_EQ(parseStringSearch("abc", false), (std::pair<std::string, bool>{"abc", false}));
	EXPECT_EQ(parseStringSearch("/a.c/", false), (std::pair<std::string, bool>{"a.c", true}));
	EXPECT_EQ(parseStringSearch("a.c", true), (std::pair<std::string, bool>{"a.c", true}));
	EXPECT_EQ(parseStringSearch("/", false), (std::pair<std::string, bool>{"/", false}));
}

TEST(StringsModelTest, FilterEdgeCases)
{
	StringsFilter f;
	StringEntry e;
	e.value = "Hello World";
	f.setTextFilter("WORLD");
	EXPECT_TRUE(f.accepts(e));
	f.setTextFilter("w.rld", true);
	EXPECT_TRUE(f.accepts(e)); // regex is case-insensitive
	f.setTextFilter("(", true);
	EXPECT_FALSE(f.regexValid());
	EXPECT_FALSE(f.accepts(e));
	f.setTextFilter("");
	f.setTypeFilter("UTF");
	e.type = StringType::UTF16BE;
	EXPECT_TRUE(f.accepts(e));
	e.type = StringType::Wide;
	EXPECT_FALSE(f.accepts(e));
}

TEST(StringsModelTest, ExplicitCategoryIsKept)
{
	StringsTable t;
	StringEntry e;
	e.value = "http://x";
	e.category = StringCategory::RegEx; // not Plain: not reclassified
	t.setStrings({e});
	EXPECT_EQ(t.entry(0).category, StringCategory::RegEx);
}

TEST(StringsModelTest, CsvQuotesValues)
{
	StringsTable t;
	StringEntry e;
	e.address = 1;
	e.value = "say \"hi\", ok";
	t.setStrings({e});
	EXPECT_EQ(
		t.exportCsv(), "Address,Type,Category,Value,Refs\n0x0000000000000001,ASCII,Plain,\"say \"\"hi\"\", ok\",0\n");
}

TEST(StringsModelTest, ConstantsFilterAndSort)
{
	ConstantsTable t;
	std::vector<ConstantEntry> cs(3);
	cs[0].value = 0x5A4D;
	cs[0].size = ConstantSize::Word;
	cs[1].value = 22;
	cs[1].size = ConstantSize::Byte;
	cs[2].value = 0x1000;
	cs[2].size = ConstantSize::Qword;
	cs[2].address = 0xabc;
	t.setConstants(cs);
	EXPECT_EQ(t.entry(2).hexStr, "0000000000001000");
	EXPECT_EQ(constantToolTip(t.entry(0), ConstColLabel), "MZ (PE) (0x5a4d)");
	EXPECT_EQ(constantLabelColor(ConstantLabel::MagicNumber), 0x89b4fau);
	ConstantsProxy p(&t);
	p.setTextFilter("ABC"); // address column
	EXPECT_EQ(p.rowCount(), 1u);
	p.setTextFilter("");
	p.sortBy(ConstColHex, true);
	EXPECT_EQ(p.entry(0).value, 22u);
	EXPECT_EQ(p.entry(2).value, 0x5A4Du);
	p.sortBy(ConstColSize, false);
	EXPECT_EQ(p.entry(0).size, ConstantSize::Qword);
	p.setKindFilter(ConstantLabel::PortNumber);
	ASSERT_EQ(p.rowCount(), 1u);
	EXPECT_EQ(p.entry(0).label, "SSH");
	EXPECT_EQ(constantKindFilterItems().size(), 9u);
	EXPECT_EQ(constantLabelKindName(ConstantLabel::SyscallNr), "Syscall");
}
