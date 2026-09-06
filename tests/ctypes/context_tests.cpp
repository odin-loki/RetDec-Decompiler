/**
* @file tests/ctypes/context_tests.cpp
* @brief Tests for the @c context module.
* @copyright (c) 2017 Avast Software, licensed under the MIT license
* @copyright (c) 2025-2026 Odin Loch trading as Imortek (modifications)
*/

#include <memory>

#include <gtest/gtest.h>

#include "retdec/ctypes/array_type.h"
#include "retdec/ctypes/context.h"
#include "retdec/ctypes/member.h"
#include "retdec/ctypes/struct_type.h"
#include "retdec/ctypes/function.h"
#include "retdec/ctypes/function_type.h"
#include "retdec/ctypes/integral_type.h"
#include "retdec/ctypes/parameter.h"
#include "retdec/ctypes/pointer_type.h"

using namespace ::testing;

namespace retdec {
namespace ctypes {
namespace tests {

class ContextTests : public Test
{
	public:
		ContextTests():
			context(std::make_shared<Context>()),
			intType(IntegralType::create(context, "int", 32)) {}

	protected:
		std::shared_ptr<Context> context;
		std::shared_ptr<Type> intType;
		Function::Parameters params;
};

#if DEATH_TESTS_ENABLED
TEST_F(ContextTests,
AddFunctionCrashesOnNullptr)
{
	EXPECT_DEATH(
		context->addFunction(nullptr),
		"violated precondition - function cannot be null"
	);
}
#endif

TEST_F(ContextTests,
HasFunctionWithNameReturnsTrueWhenFunctionIsThere)
{
	auto newF = Function::create(context, "newF", intType, params);

	EXPECT_TRUE(context->hasFunctionWithName("newF"));
}

TEST_F(ContextTests,
HasFunctionWithNameReturnsFalseWhenFunctionIsNotThere)
{
	EXPECT_FALSE(context->hasFunctionWithName("someF"));
}

TEST_F(ContextTests,
GetFunctionWithNameReturnsCorrectFunction)
{
	auto newF = Function::create(context, "newF", intType, params);

	EXPECT_EQ(newF, context->getFunctionWithName("newF"));
}

TEST_F(ContextTests,
GetFunctionWithNameReturnsNullptrWhenFunctionNotThere)
{
	EXPECT_EQ(nullptr, context->getFunctionWithName("myF"));
}

#if DEATH_TESTS_ENABLED
TEST_F(ContextTests,
AddNamedTypeCrashesOnNullptr)
{
	EXPECT_DEATH(
		context->addNamedType(nullptr),
		"violated precondition - type cannot be null"
	);
}
#endif

TEST_F(ContextTests,
HasNamedTypeReturnsTrueWhenTypeIsThere)
{
	auto newT = IntegralType::create(context, "int", 32);

	EXPECT_TRUE(context->hasNamedType("int"));
}

TEST_F(ContextTests,
HasNamedTypeReturnsFalseWhenTypeIsNotThere)
{
	EXPECT_FALSE(context->hasFunctionWithName("my_int"));
}

TEST_F(ContextTests,
GetNamedTypeReturnsCorrectType)
{
	auto newT = IntegralType::create(context, "int", 32);

	EXPECT_EQ(newT, context->getNamedType("int"));
}

TEST_F(ContextTests,
GetNamedTypeReturnsNullptrWhenTypeNotThere)
{
	EXPECT_EQ(nullptr, context->getNamedType("myF"));
}

TEST_F(ContextTests,
HasFunctionTypeReturnsTrueWhenTypeIsThere)
{
	auto newT = FunctionType::create(context, intType, {});

	EXPECT_TRUE(context->hasFunctionType(intType, {}, {}, FunctionType::VarArgness::IsNotVarArg));
}

TEST_F(ContextTests,
HasArrayTypeReturnsTrueWhenTypeIsThere)
{
	auto newT = ArrayType::create(context, intType, {10});

	EXPECT_TRUE(context->hasArrayType(intType, {10}));
}

TEST_F(ContextTests,
HasPointerTypeReturnsTrueWhenTypeIsThere)
{
	auto newT = PointerType::create(context, intType);

	EXPECT_TRUE(context->hasPointerType(intType));
}

TEST_F(ContextTests,
AddFunctionKeepsFirstOne)
{
	auto charType(IntegralType::create(context, "char", 8));
	auto f1 = Function::create(context, "f", intType, params);
	auto f2 = Function::create(context, "f", charType, params);

	context->addFunction(f1);
	context->addFunction(f2);

	EXPECT_EQ(intType, context->getFunctionWithName("f")->getReturnType());
}

TEST_F(ContextTests,
AddNamedTypeKeepsFirstOne)
{
	auto i1 = IntegralType::create(context, "int", 32);
	auto i2 = IntegralType::create(context, "int", 16);

	context->addNamedType(i1);
	context->addNamedType(i2);

	EXPECT_EQ(32, context->getNamedType("int")->getBitWidth());
}

// A self-referential type -- `struct node { struct node *next; };` -- links the
// struct to a pointer type that links straight back to the struct, both with
// shared_ptr. Destroying the Context that owns them is not enough to collect
// that cycle, so the whole subgraph used to leak. ~Context cuts the member
// edges; this test proves the objects actually die with their Context.
TEST(ContextLifetime, RecursiveStructDoesNotOutliveItsContext)
{
	std::weak_ptr<Type> weakStruct;
	std::weak_ptr<Type> weakPointer;

	{
		auto context = std::make_shared<Context>();

		auto node = StructType::create(context, "node", {});
		auto nodePtr = PointerType::create(context, node);
		node->setMembers({Member("next", nodePtr)});

		weakStruct = node;
		weakPointer = nodePtr;

		// Both are alive while the local shared_ptrs and the Context are.
		EXPECT_FALSE(weakStruct.expired());
		EXPECT_FALSE(weakPointer.expired());
	}

	EXPECT_TRUE(weakStruct.expired());
	EXPECT_TRUE(weakPointer.expired());
}

// The same must hold for a longer cycle: two structs pointing at each other.
TEST(ContextLifetime, MutuallyRecursiveStructsDoNotOutliveTheirContext)
{
	std::weak_ptr<Type> weakA;
	std::weak_ptr<Type> weakB;

	{
		auto context = std::make_shared<Context>();

		auto a = StructType::create(context, "a", {});
		auto b = StructType::create(context, "b", {});
		a->setMembers({Member("toB", PointerType::create(context, b))});
		b->setMembers({Member("toA", PointerType::create(context, a))});

		weakA = a;
		weakB = b;
	}

	EXPECT_TRUE(weakA.expired());
	EXPECT_TRUE(weakB.expired());
}

} // namespace tests
} // namespace ctypes
} // namespace retdec
