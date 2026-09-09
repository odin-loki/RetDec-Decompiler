/**
 * @file tests/config/type_tests.cpp
 * @brief Tests for the type module.
 * @copyright (c) 2017 Avast Software, licensed under the MIT license
 * @copyright (c) 2025-2026 Odin Loch trading as Imortek (modifications)
 */

#include <set>

#include <gtest/gtest.h>

#include "retdec/common/type.h"

using namespace ::testing;

namespace retdec {
namespace common {
namespace tests {

class TypeTests : public Test
{

};

TEST_F(TypeTests, WideStringSetGet)
{
	Type type("i32*");
	EXPECT_FALSE( type.isWideString() );

	type.setIsWideString(true);
	EXPECT_TRUE( type.isWideString() );

	type.setIsWideString(false);
	EXPECT_FALSE( type.isWideString() );
}

TEST_F(TypeTests, LlvmIrSetGet)
{
	Type type("i32*");
	EXPECT_EQ( "i32*", type.getLlvmIr() );
}

TEST_F(TypeTests, SameTypesAreNotLessThanEachOther)
{
	Type type1("double");
	Type type2("double");

	EXPECT_FALSE( type1 < type2 );
	EXPECT_FALSE( type2 < type1 );
}

TEST_F(TypeTests, DifferentTypesAreLessThanEachOther)
{
	Type type1("double");
	Type type2("float");

	EXPECT_TRUE( type1 < type2 );
	EXPECT_FALSE( type2 < type1 );
}

// operator< asserted that the left-hand type is defined, and went through
// getLlvmIr(), which asserts too. An ordering has to be total over every value
// the type can hold, and an undefined one arrives straight from a config file:
// a "structures" entry with no "llvmIr". The second such entry inserted into
// a set aborted the process.
TEST(UndefinedTypeOrderingTests, UndefinedTypesCompareWithoutAsserting)
{
	// A default Type is "i32", which is defined. An undefined one is what
	// serdes::deserialize produces for a "structures" entry with no "llvmIr":
	// setLlvmIr("").
	Type undef1;
	undef1.setLlvmIr("");
	Type undef2;
	undef2.setLlvmIr("");
	Type defined("%s = type { i32 }");

	ASSERT_FALSE(undef1.isDefined());

	EXPECT_FALSE(undef1 < undef2);
	EXPECT_FALSE(undef2 < undef1);
	EXPECT_TRUE(undef1 == undef2);

	// The empty representation sorts before every other, which is where an
	// undefined type belongs.
	EXPECT_TRUE(undef1 < defined);
	EXPECT_FALSE(defined < undef1);
	EXPECT_FALSE(undef1 == defined);
}

TEST(UndefinedTypeOrderingTests, ASetTakesMoreThanOneUndefinedType)
{
	Type undef;
	undef.setLlvmIr("");

	std::set<Type> types;
	types.insert(undef);
	types.insert(undef);
	types.insert(Type("%s = type { i32 }"));

	EXPECT_EQ(2u, types.size());
}

} // namespace tests
} // namespace common
} // namespace retdec
