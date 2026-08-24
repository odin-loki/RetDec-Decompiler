/**
* @file tests/bin2llvmir/optimizations/idioms_libgcc/tests/idioms_libgcc_tests.cpp
* @brief Tests for the @c IdiomsLibgcc pass.
* @copyright (c) 2017 Avast Software, licensed under the MIT license
* @copyright (c) 2025-2026 Odin Loch trading as Imortek (modifications)
*/

#include "retdec/bin2llvmir/optimizations/idioms_libgcc/idioms_libgcc.h"
#include "retdec/bin2llvmir/utils/llvm.h"
#include "bin2llvmir/utils/llvmir_tests.h"

using namespace ::testing;
using namespace llvm;

/**
 * Dummy function used to fill @c IdiomsLibgcc::Fnc2Action used in tests.
 */
void dummyFunction(llvm::CallInst* inst)
{

}

/**
 * This is *NOT* the same macro as in
 * @c frontend/bin2llvmirl/optimizations/idioms_libgcc/idioms_libgcc.cpp
 */
#define ID_FNC_PAIR(ID, FNC) \
		{ID, [] (llvm::CallInst* c) { return FNC(c); }}

namespace retdec {
namespace bin2llvmir {
namespace tests {

/**
 * @brief Tests for the @c Volatilize pass.
 */
class IdiomsLibgccTests: public LlvmIrTests
{

};

TEST_F(IdiomsLibgccTests, checkFunctionToActionMapEmptyContainerIsNotMisordered)
{
	IdiomsLibgcc::Fnc2Action f2a;

	EXPECT_FALSE(IdiomsLibgcc::checkFunctionToActionMap(f2a));
}

TEST_F(IdiomsLibgccTests, checkFunctionToActionMapDetectsMisorderedElements)
{
	IdiomsLibgcc::Fnc2Action f2a =
	{
			ID_FNC_PAIR("ab", dummyFunction),
			ID_FNC_PAIR("abcd", dummyFunction),
	};

	EXPECT_TRUE(IdiomsLibgcc::checkFunctionToActionMap(f2a));
}

TEST_F(IdiomsLibgccTests, checkFunctionToActionMapNotMisorderedElementPassTheTest)
{
	IdiomsLibgcc::Fnc2Action f2a =
	{
			ID_FNC_PAIR("abcd", dummyFunction),
			ID_FNC_PAIR("ab", dummyFunction),
	};

	EXPECT_FALSE(IdiomsLibgcc::checkFunctionToActionMap(f2a));
}

TEST_F(IdiomsLibgccTests, divsi3RegisterLoadStoreAttachesPointeeMetadata)
{
	parseInput(R"(
		@r0 = global i32 0
		@r1 = global i32 0
		declare i32 @__divsi3(i32, i32)
		define void @fnc() {
			%c = call i32 @__divsi3(i32 10, i32 3)
			ret void
		}
	)");
	auto c = config::Config::fromJsonString(R"({
		"architecture" : {
			"bitSize" : 32,
			"endian" : "little",
			"name" : "arm"
		}
	})");
	auto config = Config::fromConfig(module.get(), c);
	auto abi = AbiProvider::addAbi(module.get(), &config);
	abi->addRegister(ARM_REG_R0, getGlobalByName("r0"));
	abi->addRegister(ARM_REG_R1, getGlobalByName("r1"));

	IdiomsLibgcc pass;
	bool ret = pass.runOnModuleCustom(*module, &config, abi);
	EXPECT_TRUE(ret);

	auto* i32 = Type::getInt32Ty(context);
	auto* l0 = getNthInstruction<LoadInst>();
	auto* l1 = getNthInstruction<LoadInst>(1u);
	auto* s0 = getNthInstruction<StoreInst>();
	ASSERT_NE(nullptr, l0);
	ASSERT_NE(nullptr, l1);
	ASSERT_NE(nullptr, s0);
	EXPECT_EQ(i32, llvm_utils::getPointeeTypeMetadata(l0));
	EXPECT_EQ(i32, llvm_utils::getPointeeTypeMetadata(l1));
	EXPECT_EQ(i32, llvm_utils::getPointeeTypeMetadata(s0));
}

} // namespace tests
} // namespace bin2llvmir
} // namespace retdec
