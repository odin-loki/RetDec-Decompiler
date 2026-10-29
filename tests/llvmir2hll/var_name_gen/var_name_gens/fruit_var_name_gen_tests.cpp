/**
* @file tests/llvmir2hll/var_name_gen/var_name_gens/fruit_var_name_gen_tests.cpp
* @brief Tests for the @c fruit_var_name_gen module.
* @copyright (c) 2025-2026 Odin Loch trading as Imortek
*/

#include <set>
#include <string>

#include <gtest/gtest.h>

#include "retdec/llvmir2hll/var_name_gen/var_name_gens/fruit_var_name_gen.h"

namespace retdec {
namespace llvmir2hll {
namespace tests {

TEST(FruitVarNameGenTests, NamesAreDistinctUntilTheListWrapsAround) {
	auto gen = FruitVarNameGen::create("v_");
	std::set<std::string> seen;
	const std::string first = gen->getNextVarName();
	seen.insert(first);
	std::string name;
	while ((name = gen->getNextVarName()) != first) {
		EXPECT_TRUE(seen.insert(name).second) << "duplicate name " << name;
		ASSERT_LT(seen.size(), 1000u);
	}
	// "mape" is the last entry and must be handed out before wrapping.
	EXPECT_EQ(1u, seen.count("v_mape"));
}

TEST(FruitVarNameGenTests, RestartReturnsTheFirstName) {
	auto gen = FruitVarNameGen::create();
	const std::string first = gen->getNextVarName();
	gen->getNextVarName();
	gen->restart();
	EXPECT_EQ(first, gen->getNextVarName());
}

} // namespace tests
} // namespace llvmir2hll
} // namespace retdec
