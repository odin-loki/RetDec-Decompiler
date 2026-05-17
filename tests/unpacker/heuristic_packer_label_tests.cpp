/**
 * @file tests/unpacker/heuristic_packer_label_tests.cpp
 * @brief Tests for heuristic-only packer name detection (P1.3).
 */

#include <gtest/gtest.h>

#include "retdec/unpackertool/unpackertool.h"

using namespace retdec::unpackertool;

namespace {

TEST(HeuristicPackerLabel, StructuralEntropyName)
{
	EXPECT_TRUE(isHeuristicOnlyPackerLabel("Unknown packer (structural entropy)"));
}

TEST(HeuristicPackerLabel, PrefixOnly)
{
	EXPECT_TRUE(isHeuristicOnlyPackerLabel("Unknown packer (anything)"));
}

TEST(HeuristicPackerLabel, SignaturePackerNotHeuristicLabel)
{
	EXPECT_FALSE(isHeuristicOnlyPackerLabel("UPX"));
	EXPECT_FALSE(isHeuristicOnlyPackerLabel("MPRESS"));
	EXPECT_FALSE(isHeuristicOnlyPackerLabel("Unknown UPX"));
}

} // namespace
