/**
 * @file tests/cpdetect/packer_section_hints_tests.cpp
 * @brief Unit tests for packer section name hints (P1.2).
 */

#include <gtest/gtest.h>

#include "retdec/cpdetect/packer_section_hints.h"

using namespace retdec::cpdetect;

namespace {

TEST(PackerSectionHints, RecognizesCommonPrefixes)
{
	EXPECT_TRUE(sectionNameSuggestsEmbeddedPackerSection(".upx0"));
	EXPECT_TRUE(sectionNameSuggestsEmbeddedPackerSection(".upx1"));
	EXPECT_TRUE(sectionNameSuggestsEmbeddedPackerSection(".nsp0"));
	EXPECT_TRUE(sectionNameSuggestsEmbeddedPackerSection(".aspack"));
	EXPECT_TRUE(sectionNameSuggestsEmbeddedPackerSection(".mpress0"));
	EXPECT_TRUE(sectionNameSuggestsEmbeddedPackerSection(".packed"));
	EXPECT_TRUE(sectionNameSuggestsEmbeddedPackerSection(".themida"));
	EXPECT_TRUE(sectionNameSuggestsEmbeddedPackerSection(".tmd0"));
	EXPECT_TRUE(sectionNameSuggestsEmbeddedPackerSection(".petite"));
	EXPECT_TRUE(sectionNameSuggestsEmbeddedPackerSection(".fsg0"));
	EXPECT_TRUE(sectionNameSuggestsEmbeddedPackerSection(".neolite"));
	EXPECT_TRUE(sectionNameSuggestsEmbeddedPackerSection(".vmp0"));
	EXPECT_TRUE(sectionNameSuggestsEmbeddedPackerSection(".enigma1"));
	EXPECT_TRUE(sectionNameSuggestsEmbeddedPackerSection(".pec1"));
	EXPECT_TRUE(sectionNameSuggestsEmbeddedPackerSection(".svkp0"));
	EXPECT_TRUE(sectionNameSuggestsEmbeddedPackerSection(".pebundle"));
	EXPECT_TRUE(sectionNameSuggestsEmbeddedPackerSection(".upack1"));
	EXPECT_TRUE(sectionNameSuggestsEmbeddedPackerSection(".npack"));
	EXPECT_TRUE(sectionNameSuggestsEmbeddedPackerSection(".kkrunchy"));
	EXPECT_TRUE(sectionNameSuggestsEmbeddedPackerSection(".rlpack"));
	EXPECT_TRUE(sectionNameSuggestsEmbeddedPackerSection(".adata"));
	EXPECT_TRUE(sectionNameSuggestsEmbeddedPackerSection(".udata"));
	EXPECT_TRUE(sectionNameSuggestsEmbeddedPackerSection(".mew0"));
	EXPECT_TRUE(sectionNameSuggestsEmbeddedPackerSection(".mew1"));
	EXPECT_TRUE(sectionNameSuggestsEmbeddedPackerSection(".dsstext"));
	EXPECT_TRUE(sectionNameSuggestsEmbeddedPackerSection(".dyndata"));
	EXPECT_TRUE(sectionNameSuggestsEmbeddedPackerSection(".alien0"));
	EXPECT_TRUE(sectionNameSuggestsEmbeddedPackerSection(".spin0"));
	EXPECT_TRUE(sectionNameSuggestsEmbeddedPackerSection(".epack"));
	EXPECT_TRUE(sectionNameSuggestsEmbeddedPackerSection(".shrink0"));
	EXPECT_TRUE(sectionNameSuggestsEmbeddedPackerSection(".exef"));
	EXPECT_TRUE(sectionNameSuggestsEmbeddedPackerSection(".pklite"));
	EXPECT_TRUE(sectionNameSuggestsEmbeddedPackerSection(".xt0"));
	EXPECT_TRUE(sectionNameSuggestsEmbeddedPackerSection(".winup0"));
	EXPECT_TRUE(sectionNameSuggestsEmbeddedPackerSection(".pearmor"));
	EXPECT_TRUE(sectionNameSuggestsEmbeddedPackerSection(".yoda0"));
	EXPECT_TRUE(sectionNameSuggestsEmbeddedPackerSection(".bob0"));
	EXPECT_TRUE(sectionNameSuggestsEmbeddedPackerSection(".rpcrypt"));
	EXPECT_TRUE(sectionNameSuggestsEmbeddedPackerSection(".telock"));
	EXPECT_TRUE(sectionNameSuggestsEmbeddedPackerSection(".pediy"));
	EXPECT_TRUE(sectionNameSuggestsEmbeddedPackerSection(".dark0"));
	EXPECT_TRUE(sectionNameSuggestsEmbeddedPackerSection(".fish0"));
	EXPECT_TRUE(sectionNameSuggestsEmbeddedPackerSection(".packman0"));
	EXPECT_TRUE(sectionNameSuggestsEmbeddedPackerSection(".wwpack0"));
	// NSPack-style names are matched by the existing `.nsp*` prefix (Neolite / NSPack family).
	EXPECT_TRUE(sectionNameSuggestsEmbeddedPackerSection(".nspack0"));
}

TEST(PackerSectionHints, TypicalCodeSectionNameButNotMarkedCode)
{
	EXPECT_TRUE(typicalCodeSectionNameButNotMarkedCode(false, ".text"));
	EXPECT_TRUE(typicalCodeSectionNameButNotMarkedCode(false, ".textfoo"));
	EXPECT_TRUE(typicalCodeSectionNameButNotMarkedCode(false, "code"));
	EXPECT_FALSE(typicalCodeSectionNameButNotMarkedCode(true, ".text"));
	EXPECT_FALSE(typicalCodeSectionNameButNotMarkedCode(false, ".data"));
	EXPECT_FALSE(typicalCodeSectionNameButNotMarkedCode(false, ".rsrc"));
}

TEST(PackerSectionHints, PeExecutableSectionNamedLikeWritableData)
{
	EXPECT_TRUE(peExecutableSectionNamedLikeWritableData(true, true, ".data"));
	EXPECT_TRUE(peExecutableSectionNamedLikeWritableData(true, true, ".dataro"));
	EXPECT_TRUE(peExecutableSectionNamedLikeWritableData(true, true, ".bss"));
	EXPECT_FALSE(peExecutableSectionNamedLikeWritableData(false, true, ".data"));
	EXPECT_FALSE(peExecutableSectionNamedLikeWritableData(true, false, ".data"));
	EXPECT_FALSE(peExecutableSectionNamedLikeWritableData(true, true, ".text"));
	EXPECT_FALSE(peExecutableSectionNamedLikeWritableData(true, true, ".rdata"));
}

TEST(PackerSectionHints, RejectsBenignOrAmbiguous)
{
	EXPECT_FALSE(sectionNameSuggestsEmbeddedPackerSection(".text"));
	EXPECT_FALSE(sectionNameSuggestsEmbeddedPackerSection(".data"));
	EXPECT_FALSE(sectionNameSuggestsEmbeddedPackerSection("unpacked"));
	EXPECT_FALSE(sectionNameSuggestsEmbeddedPackerSection(".idata"));
	EXPECT_FALSE(sectionNameSuggestsEmbeddedPackerSection(".edata"));
	// Normal PE base relocations — not a packer fingerprint
	EXPECT_FALSE(sectionNameSuggestsEmbeddedPackerSection(".reloc"));
	EXPECT_FALSE(sectionNameSuggestsEmbeddedPackerSection(".rsrc"));
	// Substring "themida" without packer-style prefix — no longer matched
	EXPECT_FALSE(sectionNameSuggestsEmbeddedPackerSection("xthemiday"));
}

TEST(PackerSectionHints, PeIatThunkFillPercentRejectsInvalid)
{
	EXPECT_FALSE(peIatThunkFillPercent(0, 100, false).has_value());
	EXPECT_FALSE(peIatThunkFillPercent(1, 0, false).has_value());
	// IAT directory smaller than one 32-bit thunk
	EXPECT_FALSE(peIatThunkFillPercent(10, 3, false).has_value());
	EXPECT_FALSE(peIatThunkFillPercent(10, 7, true).has_value());
}

TEST(PackerSectionHints, PeIatThunkFillPercentHalfFull32)
{
	auto v = peIatThunkFillPercent(10, 80, false);
	ASSERT_TRUE(v.has_value());
	EXPECT_EQ(50u, *v);
}

TEST(PackerSectionHints, PeIatThunkFillPercentCapsAt100)
{
	ASSERT_TRUE(peIatThunkFillPercent(1, 4, false).has_value());
	EXPECT_EQ(100u, *peIatThunkFillPercent(1, 4, false));
	auto heavy = peIatThunkFillPercent(1000, 4, false);
	ASSERT_TRUE(heavy.has_value());
	EXPECT_EQ(100u, *heavy);
}

TEST(PackerSectionHints, TypicalCodeSectionNames)
{
	EXPECT_TRUE(sectionNameLooksLikeTypicalCodeSection(".text"));
	EXPECT_TRUE(sectionNameLooksLikeTypicalCodeSection("__text"));
	EXPECT_TRUE(sectionNameLooksLikeTypicalCodeSection("code"));
	EXPECT_TRUE(sectionNameLooksLikeTypicalCodeSection(".textfoo"));
	EXPECT_TRUE(sectionNameLooksLikeTypicalCodeSection("__text_init"));
	EXPECT_TRUE(sectionNameLooksLikeTypicalCodeSection("__text,__text"));
}

TEST(PackerSectionHints, AtypicalCodeSectionNames)
{
	EXPECT_FALSE(sectionNameLooksLikeTypicalCodeSection(".upx0"));
	EXPECT_FALSE(sectionNameLooksLikeTypicalCodeSection(".data"));
	EXPECT_FALSE(sectionNameLooksLikeTypicalCodeSection("xtext"));
}

} // namespace
