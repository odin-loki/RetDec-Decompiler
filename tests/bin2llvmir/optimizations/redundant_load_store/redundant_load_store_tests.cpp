/**
 * @file tests/bin2llvmir/optimizations/redundant_load_store/redundant_load_store_tests.cpp
 * @brief Tests for the @c RedundantLoadStoreElim pass.
 * @copyright (c) 2025-2026 Odin Loch trading as Imortek, licensed under the MIT license
 *
 * The pass is in the shipped pipeline (retdec-redundant-load-store) and had no
 * tests. Every case below is a thing it used to get wrong.
 */

#include "bin2llvmir/utils/llvmir_tests.h"
#include "retdec/bin2llvmir/optimizations/redundant_load_store/redundant_load_store.h"

using namespace ::testing;
using namespace llvm;

namespace retdec {
namespace bin2llvmir {
namespace tests {

class RedundantLoadStoreTests : public LlvmIrTests {
protected:
	RedundantLoadStoreElim pass;
};

//
// What the pass is for
//

TEST_F(RedundantLoadStoreTests, aStoredValueIsForwardedToTheLoad)
{
	parseInput(R"(
		@g = global i32 0
		define i32 @fnc(i32 %x) {
			store i32 %x, i32* @g
			%a = load i32, i32* @g
			ret i32 %a
		}
	)");

	bool ret = pass.runOnModule(*module);

	std::string exp = R"(
		@g = global i32 0
		define i32 @fnc(i32 %x) {
			store i32 %x, i32* @g
			ret i32 %x
		}
	)";
	checkModuleAgainstExpectedIr(exp);
	EXPECT_TRUE(ret);
}

//
// A narrower store does not overwrite a wider one
//
// The dead-store map was keyed on the pointer alone, so `store i32` was taken
// to cover the whole of a preceding `store i64` and deleted it, losing the top
// four bytes.
//

TEST_F(RedundantLoadStoreTests, aNarrowStoreDoesNotKillAWiderOne)
{
	// Both stores must name the pointer the same way, or the dead-store map
	// never pairs them up and the size check is not the thing under test.
	parseInput(R"(
		@g = global i64 0
		define void @fnc() {
			store i64 -1, ptr @g
			store i32 0, ptr @g
			ret void
		}
	)");

	pass.runOnModule(*module);

	auto* f = module->getFunction("fnc");
	unsigned stores = 0;
	for (auto& i: f->front())
	{
		if (isa<StoreInst>(&i))
		{
			++stores;
		}
	}
	EXPECT_EQ(2u, stores) << "the i64 store is only half covered by the i32 one";
}

//
// Volatile and atomic accesses are the point of the instruction
//

TEST_F(RedundantLoadStoreTests, aVolatileLoadIsNotAnsweredFromAStore)
{
	parseInput(R"(
		@mmio = global i32 0
		define i32 @fnc() {
			store i32 1, i32* @mmio
			%a = load volatile i32, i32* @mmio
			ret i32 %a
		}
	)");

	bool ret = pass.runOnModule(*module);

	std::string exp = R"(
		@mmio = global i32 0
		define i32 @fnc() {
			store i32 1, i32* @mmio
			%a = load volatile i32, i32* @mmio
			ret i32 %a
		}
	)";
	checkModuleAgainstExpectedIr(exp);
	EXPECT_FALSE(ret);
}

TEST_F(RedundantLoadStoreTests, aVolatileStoreIsNotDead)
{
	parseInput(R"(
		@mmio = global i32 0
		define void @fnc() {
			store volatile i32 1, i32* @mmio
			store volatile i32 2, i32* @mmio
			ret void
		}
	)");

	bool ret = pass.runOnModule(*module);

	std::string exp = R"(
		@mmio = global i32 0
		define void @fnc() {
			store volatile i32 1, i32* @mmio
			store volatile i32 2, i32* @mmio
			ret void
		}
	)";
	checkModuleAgainstExpectedIr(exp);
	EXPECT_FALSE(ret);
}

//
// A read makes the preceding store observable
//
// callMayWrite answered false for a readonly callee and that one predicate
// gated both maps, so the store before it was treated as dead -- even though
// reading it is exactly what the callee does.
//

TEST_F(RedundantLoadStoreTests, aReadOnlyCallKeepsThePrecedingStore)
{
	parseInput(R"(
		@g = global i32 0
		declare void @observe() readonly
		define void @fnc(i32 %x) {
			store i32 1, i32* @g
			call void @observe()
			store i32 %x, i32* @g
			ret void
		}
	)");

	pass.runOnModule(*module);

	auto* f = module->getFunction("fnc");
	unsigned stores = 0;
	for (auto& i: f->front())
	{
		if (isa<StoreInst>(&i))
		{
			++stores;
		}
	}
	EXPECT_EQ(2u, stores) << "@observe reads the value the first store wrote";
}

//
// Two names for one address
//
// There is no alias analysis here and the map is keyed on the pointer Value*,
// so a store through a second name for the same location left the first name's
// remembered value in place.
//

TEST_F(RedundantLoadStoreTests, aStoreThroughAnotherNameInvalidates)
{
	parseInput(R"(
		@g = global i32 0
		define i32 @fnc() {
			%p = getelementptr i32, i32* @g, i32 0
			store i32 1, i32* @g
			store i32 2, i32* %p
			%a = load i32, i32* @g
			ret i32 %a
		}
	)");

	pass.runOnModule(*module);

	auto* f = module->getFunction("fnc");
	auto* ret = cast<ReturnInst>(f->front().getTerminator());
	auto* c = dyn_cast<ConstantInt>(ret->getOperand(0));
	EXPECT_FALSE(c && c->equalsInt(1)) << "the load was answered with the value the FIRST store wrote";
}

} // namespace tests
} // namespace bin2llvmir
} // namespace retdec
