/**
 * @file tests/bin2llvmir/optimizations/decoder/decoder_linking_section_names_tests.cpp
 * @brief Unit tests for PLT/GOT-like section name helpers (P1.5).
 */

#include <gtest/gtest.h>

#include "retdec/bin2llvmir/optimizations/decoder/decoder_linking_section_names.h"

using namespace retdec::bin2llvmir;

namespace {

TEST(DecoderLinkingSectionNames, PltLikeRecognizesCommonNames)
{
	EXPECT_TRUE(sectionNameIsPltLike(".plt"));
	EXPECT_TRUE(sectionNameIsPltLike(".plt.sec"));
	EXPECT_TRUE(sectionNameIsPltLike(".plt.got"));
	EXPECT_TRUE(sectionNameIsPltLike(".plt.extra"));
	EXPECT_FALSE(sectionNameIsPltLike(".got.plt"));
	EXPECT_FALSE(sectionNameIsPltLike(".got"));
	EXPECT_FALSE(sectionNameIsPltLike(".text"));
}

TEST(DecoderLinkingSectionNames, GotLikeRecognizesElfAndMachOStyle)
{
	EXPECT_TRUE(segmentNameIsGlobalOffsetTableLike(".got"));
	EXPECT_TRUE(segmentNameIsGlobalOffsetTableLike(".got.plt"));
	EXPECT_TRUE(segmentNameIsGlobalOffsetTableLike(".got.sec"));
	EXPECT_TRUE(segmentNameIsGlobalOffsetTableLike("__DATA,__got"));
	EXPECT_TRUE(segmentNameIsGlobalOffsetTableLike("__DATA_CONST,__got"));
	EXPECT_TRUE(segmentNameIsGlobalOffsetTableLike("__DATA,__la_symbol_ptr"));
	EXPECT_TRUE(segmentNameIsGlobalOffsetTableLike("__DATA,__nl_symbol_ptr"));
	EXPECT_FALSE(segmentNameIsGlobalOffsetTableLike(".idata"));
	EXPECT_FALSE(segmentNameIsGlobalOffsetTableLike(".text"));
	EXPECT_FALSE(segmentNameIsGlobalOffsetTableLike(".plt"));
}

TEST(DecoderLinkingSectionNames, PeImportDataLike)
{
	EXPECT_TRUE(segmentNameIsPeImportDataLike(".idata"));
	EXPECT_TRUE(segmentNameIsPeImportDataLike(".idata$2"));
	EXPECT_FALSE(segmentNameIsPeImportDataLike(".got"));
	EXPECT_FALSE(segmentNameIsPeImportDataLike(".text"));
}

} // namespace
