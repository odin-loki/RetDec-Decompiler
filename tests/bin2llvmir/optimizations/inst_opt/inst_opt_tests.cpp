/**
* @file tests/bin2llvmir/optimizations/inst_opt/inst_opt_tests.cpp
* @brief Tests for the @c inst_opt::optimize().
* @copyright (c) 2017 Avast Software, licensed under the MIT license
* @copyright (c) 2025-2026 Odin Loch trading as Imortek (modifications)
*/

#include "bin2llvmir/utils/llvmir_tests.h"
#include "retdec/bin2llvmir/optimizations/inst_opt/inst_opt.h"
#include "retdec/bin2llvmir/utils/llvm.h"

using namespace ::testing;
using namespace llvm;

namespace retdec {
namespace bin2llvmir {
namespace tests {

/**
 * @brief Tests for the @c inst_opt::optimize().
 */
class OptimizeTests: public LlvmIrTests
{

};

//
// no optimization
//

TEST_F(OptimizeTests, noOptimizationReturnsFalse)
{
	parseInput(R"(
		@reg = global i32 0
		define i32 @fnc() {
			%a = load i32, i32* @reg
			ret i32 %a
		}
	)");
	auto* i = getInstructionByName("a");

	bool ret = inst_opt::optimize(i);

	std::string exp = R"(
		@reg = global i32 0
		define i32 @fnc() {
			%a = load i32, i32* @reg
			ret i32 %a
		}
	)";
	checkModuleAgainstExpectedIr(exp);
	EXPECT_FALSE(ret);
}

//
// add zero
//

TEST_F(OptimizeTests, addValZero)
{
	parseInput(R"(
		@reg = global i32 0
		define i32 @fnc() {
			%a = load i32, i32* @reg
			%b = add i32 %a, 0
			ret i32 %b
		}
	)");
	auto* i = getInstructionByName("b");

	bool ret = inst_opt::optimize(i);

	std::string exp = R"(
		@reg = global i32 0
		define i32 @fnc() {
			%a = load i32, i32* @reg
			ret i32 %a
		}
	)";
	checkModuleAgainstExpectedIr(exp);
	EXPECT_TRUE(ret);
}

TEST_F(OptimizeTests, addZeroVal)
{
	parseInput(R"(
		@reg = global i32 0
		define i32 @fnc() {
			%a = load i32, i32* @reg
			%b = add i32 0, %a
			ret i32 %b
		}
	)");
	auto* i = getInstructionByName("b");

	bool ret = inst_opt::optimize(i);

	std::string exp = R"(
		@reg = global i32 0
		define i32 @fnc() {
			%a = load i32, i32* @reg
			ret i32 %a
		}
	)";
	checkModuleAgainstExpectedIr(exp);
	EXPECT_TRUE(ret);
}

TEST_F(OptimizeTests, addValVal)
{
	parseInput(R"(
		@reg = global i32 0
		define i32 @fnc() {
			%a = load i32, i32* @reg
			%b = add i32 %a, 10
			ret i32 %b
		}
	)");
	auto* i = getInstructionByName("b");

	bool ret = inst_opt::optimize(i);

	std::string exp = R"(
		@reg = global i32 0
		define i32 @fnc() {
			%a = load i32, i32* @reg
			%b = add i32 %a, 10
			ret i32 %b
		}
	)";
	checkModuleAgainstExpectedIr(exp);
	EXPECT_FALSE(ret);
}

//
// sub zero
//

TEST_F(OptimizeTests, subValZero)
{
	parseInput(R"(
		@reg = global i32 0
		define i32 @fnc() {
			%a = load i32, i32* @reg
			%b = sub i32 %a, 0
			ret i32 %b
		}
	)");
	auto* i = getInstructionByName("b");

	bool ret = inst_opt::optimize(i);

	std::string exp = R"(
		@reg = global i32 0
		define i32 @fnc() {
			%a = load i32, i32* @reg
			ret i32 %a
		}
	)";
	checkModuleAgainstExpectedIr(exp);
	EXPECT_TRUE(ret);
}

TEST_F(OptimizeTests, subValVal)
{
	parseInput(R"(
		@reg = global i32 0
		define i32 @fnc() {
			%a = load i32, i32* @reg
			%b = sub i32 %a, 10
			ret i32 %b
		}
	)");
	auto* i = getInstructionByName("b");

	bool ret = inst_opt::optimize(i);

	std::string exp = R"(
		@reg = global i32 0
		define i32 @fnc() {
			%a = load i32, i32* @reg
			%b = sub i32 %a, 10
			ret i32 %b
		}
	)";
	checkModuleAgainstExpectedIr(exp);
	EXPECT_FALSE(ret);
}

//
// trunc zext
//

TEST_F(OptimizeTests, truncZext8)
{
	parseInput(R"(
		@reg = global i32 0
		define i32 @fnc() {
			%a = load i32, i32* @reg
			%b = trunc i32 %a to i8
			%c = zext i8 %b to i32
			ret i32 %c
		}
	)");
	auto* i = getInstructionByName("c");

	bool ret = inst_opt::optimize(i);

	std::string exp = R"(
		@reg = global i32 0
		define i32 @fnc() {
			%a = load i32, i32* @reg
			%c = and i32 %a, 255
			ret i32 %c
		}
	)";
	checkModuleAgainstExpectedIr(exp);
	EXPECT_TRUE(ret);
}

TEST_F(OptimizeTests, truncZext16)
{
	parseInput(R"(
		@reg = global i32 0
		define i32 @fnc() {
			%a = load i32, i32* @reg
			%b = trunc i32 %a to i16
			%c = zext i16 %b to i32
			ret i32 %c
		}
	)");
	auto* i = getInstructionByName("c");

	bool ret = inst_opt::optimize(i);

	std::string exp = R"(
		@reg = global i32 0
		define i32 @fnc() {
			%a = load i32, i32* @reg
			%c = and i32 %a, 65535
			ret i32 %c
		}
	)";
	checkModuleAgainstExpectedIr(exp);
	EXPECT_TRUE(ret);
}

//
// xor X, X
//

TEST_F(OptimizeTests, xorXX)
{
	parseInput(R"(
		define i32 @fnc() {
			%a = xor i32 10, 10
			ret i32 %a
		}
	)");
	auto* i = getInstructionByName("a");

	bool ret = inst_opt::optimize(i);

	std::string exp = R"(
		define i32 @fnc() {
			ret i32 0
		}
	)";
	checkModuleAgainstExpectedIr(exp);
	EXPECT_TRUE(ret);
}

//
// xor load X, load X
//

TEST_F(OptimizeTests, xorLoadX)
{
	parseInput(R"(
		@reg = global i32 0
		define i32 @fnc() {
			%a = load i32, i32* @reg
			%b = xor i32 %a, %a
			ret i32 %b
		}
	)");
	auto* i = getInstructionByName("b");

	bool ret = inst_opt::optimize(i);

	std::string exp = R"(
		@reg = global i32 0
		define i32 @fnc() {
			ret i32 0
		}
	)";
	checkModuleAgainstExpectedIr(exp);
	EXPECT_TRUE(ret);
}

TEST_F(OptimizeTests, xorLoadXLoadX)
{
	parseInput(R"(
		@reg = global i32 0
		define i32 @fnc() {
			%a = load i32, i32* @reg
			%b = load i32, i32* @reg
			%c = xor i32 %a, %b
			ret i32 %c
		}
	)");
	auto* i = getInstructionByName("c");

	bool ret = inst_opt::optimize(i);

	std::string exp = R"(
		@reg = global i32 0
		define i32 @fnc() {
			ret i32 0
		}
	)";
	checkModuleAgainstExpectedIr(exp);
	EXPECT_TRUE(ret);
}

//
// or X, X
//

TEST_F(OptimizeTests, orXX)
{
	parseInput(R"(
		define i32 @fnc() {
			%a = or i32 10, 10
			ret i32 %a
		}
	)");
	auto* i = getInstructionByName("a");

	bool ret = inst_opt::optimize(i);

	std::string exp = R"(
		define i32 @fnc() {
			ret i32 10
		}
	)";
	checkModuleAgainstExpectedIr(exp);
	EXPECT_TRUE(ret);
}

//
// xor i1 X, Y
//

TEST_F(OptimizeTests, xor_i1_xy)
{
	parseInput(R"(
		@reg = global i1 1
		define i1 @fnc() {
			%a = load i1, i1* @reg
			%b = xor i1 %a, 1
			ret i1 %b
		}
	)");
	auto* i = getInstructionByName("b");

	bool ret = inst_opt::optimize(i);

	std::string exp = R"(
		@reg = global i1 1
		define i1 @fnc() {
			%a = load i1, i1* @reg
			%b = icmp ne i1 %a, 1
			ret i1 %b
		}
	)";
	checkModuleAgainstExpectedIr(exp);
	EXPECT_TRUE(ret);
}

//
// and i1 X, Y
//

// `and i1 x, true` is x. This used to become `icmp eq i1 %a, 1`, which is also
// x, which is why picking 1 for the constant made the old rewrite look right.
TEST_F(OptimizeTests, and_i1_xTrue)
{
	parseInput(R"(
		@reg = global i1 1
		define i1 @fnc() {
			%a = load i1, i1* @reg
			%b = and i1 %a, 1
			ret i1 %b
		}
	)");
	auto* i = getInstructionByName("b");

	bool ret = inst_opt::optimize(i);

	std::string exp = R"(
		@reg = global i1 1
		define i1 @fnc() {
			%a = load i1, i1* @reg
			ret i1 %a
		}
	)";
	checkModuleAgainstExpectedIr(exp);
	EXPECT_TRUE(ret);
}

TEST_F(OptimizeTests, and_i1_xFalse)
{
	parseInput(R"(
		@reg = global i1 1
		define i1 @fnc() {
			%a = load i1, i1* @reg
			%b = and i1 %a, 0
			ret i1 %b
		}
	)");
	auto* i = getInstructionByName("b");

	bool ret = inst_opt::optimize(i);

	std::string exp = R"(
		@reg = global i1 1
		define i1 @fnc() {
			%a = load i1, i1* @reg
			ret i1 false
		}
	)";
	checkModuleAgainstExpectedIr(exp);
	EXPECT_TRUE(ret);
}

// The case the old rewrite got wrong, and that no test reached: two operands
// neither of which is a constant. `and i1 0, 0` is 0 and `icmp eq i1 0, 0` is
// 1, so there is no comparison to rewrite this into and it must be left alone.
TEST_F(OptimizeTests, and_i1_twoValuesIsNotAComparison)
{
	parseInput(R"(
		@reg1 = global i1 1
		@reg2 = global i1 1
		define i1 @fnc() {
			%a = load i1, i1* @reg1
			%b = load i1, i1* @reg2
			%c = and i1 %a, %b
			ret i1 %c
		}
	)");
	auto* i = getInstructionByName("c");

	bool ret = inst_opt::optimize(i);

	std::string exp = R"(
		@reg1 = global i1 1
		@reg2 = global i1 1
		define i1 @fnc() {
			%a = load i1, i1* @reg1
			%b = load i1, i1* @reg2
			%c = and i1 %a, %b
			ret i1 %c
		}
	)";
	checkModuleAgainstExpectedIr(exp);
	EXPECT_FALSE(ret);
}

//
// and X, X
//

TEST_F(OptimizeTests, andXX)
{
	parseInput(R"(
		define i32 @fnc() {
			%a = and i32 10, 10
			ret i32 %a
		}
	)");
	auto* i = getInstructionByName("a");

	bool ret = inst_opt::optimize(i);

	std::string exp = R"(
		define i32 @fnc() {
			ret i32 10
		}
	)";
	checkModuleAgainstExpectedIr(exp);
	EXPECT_TRUE(ret);
}

//
// or load X, load X
//

TEST_F(OptimizeTests, orLoadX)
{
	parseInput(R"(
		@reg = global i32 0
		define i32 @fnc() {
			%a = load i32, i32* @reg
			%b = or i32 %a, %a
			ret i32 %b
		}
	)");
	auto* i = getInstructionByName("b");

	bool ret = inst_opt::optimize(i);

	std::string exp = R"(
		@reg = global i32 0
		define i32 @fnc() {
			%a = load i32, i32* @reg
			ret i32 %a
		}
	)";
	checkModuleAgainstExpectedIr(exp);
	EXPECT_TRUE(ret);
}

TEST_F(OptimizeTests, orLoadXLoadX)
{
	parseInput(R"(
		@reg = global i32 0
		define i32 @fnc() {
			%a = load i32, i32* @reg
			%b = load i32, i32* @reg
			%c = or i32 %a, %b
			ret i32 %c
		}
	)");
	auto* i = getInstructionByName("c");

	bool ret = inst_opt::optimize(i);

	std::string exp = R"(
		@reg = global i32 0
		define i32 @fnc() {
			%a = load i32, i32* @reg
			ret i32 %a
		}
	)";
	checkModuleAgainstExpectedIr(exp);
	EXPECT_TRUE(ret);
}

//
// and load X, load X
//

TEST_F(OptimizeTests, andLoadX)
{
	parseInput(R"(
		@reg = global i32 0
		define i32 @fnc() {
			%a = load i32, i32* @reg
			%b = and i32 %a, %a
			ret i32 %b
		}
	)");
	auto* i = getInstructionByName("b");

	bool ret = inst_opt::optimize(i);

	std::string exp = R"(
		@reg = global i32 0
		define i32 @fnc() {
			%a = load i32, i32* @reg
			ret i32 %a
		}
	)";
	checkModuleAgainstExpectedIr(exp);
	EXPECT_TRUE(ret);
}

TEST_F(OptimizeTests, andLoadXLoadX)
{
	parseInput(R"(
		@reg = global i32 0
		define i32 @fnc() {
			%a = load i32, i32* @reg
			%b = load i32, i32* @reg
			%c = and i32 %a, %b
			ret i32 %c
		}
	)");
	auto* i = getInstructionByName("c");

	bool ret = inst_opt::optimize(i);

	std::string exp = R"(
		@reg = global i32 0
		define i32 @fnc() {
			%a = load i32, i32* @reg
			ret i32 %a
		}
	)";
	checkModuleAgainstExpectedIr(exp);
	EXPECT_TRUE(ret);
}

//
// addSequence()
//

TEST_F(OptimizeTests, addSequence)
{
	parseInput(R"(
		@reg = global i32 0
		define i32 @fnc() {
			%a = load i32, i32* @reg
			%b = add i32 %a, 1
			%c = add i32 %b, 2
			ret i32 %c
		}
	)");
	auto* i = getInstructionByName("c");

	bool ret = inst_opt::optimize(i);

	std::string exp = R"(
		@reg = global i32 0
		define i32 @fnc() {
			%a = load i32, i32* @reg
			%c = add i32 %a, 3
			ret i32 %c
		}
	)";
	checkModuleAgainstExpectedIr(exp);
	EXPECT_TRUE(ret);
}

//==============================================================================

//
// castSequence
//

TEST_F(OptimizeTests, castSequence_ptr_int_ptr)
{
	parseInput(R"(
		@r = global i32* null
		declare i32 @print (i8*)
		define void @func() {
			%a = load i32*, i32** @r
			%b = ptrtoint i32* %a to i32
			%c = inttoptr i32 %b to i8*
			%d = call i32 @print(i8* %c)
			ret void
		}
	)");
	auto* i = getInstructionByName("c");

	bool ret = inst_opt::optimize(i);

	std::string exp = R"(
		@r = global i32* null
		declare i32 @print (i8*)
		define void @func() {
			%a = load i32*, i32** @r
			%d = call i32 @print(i8* %a)
			ret void
		}
	)";
	checkModuleAgainstExpectedIr(exp);
	EXPECT_TRUE(ret);
}

TEST_F(OptimizeTests, castSequencePtrCastAttachesPointeeMetadata)
{
	parseInput(R"(
		declare i32 @print(ptr)
		define void @func() {
			%buf = alloca i8
			%b = ptrtoint ptr %buf to i32
			%c = inttoptr i32 %b to ptr
			%d = call i32 @print(ptr %c)
			ret void
		}
	)");
	auto* i = getInstructionByName("c");

	bool ret = inst_opt::optimize(i);
	EXPECT_TRUE(ret);

	EXPECT_EQ(nullptr, getNthInstruction<IntToPtrInst>());
	auto* buf = getNthInstruction<AllocaInst>();
	ASSERT_NE(nullptr, buf);
	auto* i8 = Type::getInt8Ty(context);
	EXPECT_EQ(i8, llvm_utils::pointeeType(buf));
}

TEST_F(OptimizeTests, storeToBitcastPointerAttachesPointeeMetadata)
{
	parseInput(R"(
		@gv = global i32 0
		define void @func(float %v) {
			%p = bitcast i32* @gv to float*
			store float %v, float* %p
			ret void
		}
	)");
	auto* st = getNthInstruction<StoreInst>();

	bool ret = inst_opt::optimize(st);
	EXPECT_TRUE(ret);

	auto* ns = getNthInstruction<StoreInst>();
	ASSERT_NE(nullptr, ns);
	auto* i32 = Type::getInt32Ty(context);
	EXPECT_EQ(i32, llvm_utils::getPointeeTypeMetadata(ns));
	EXPECT_EQ(i32, llvm_utils::pointeeType(ns->getPointerOperand()));
}

TEST_F(OptimizeTests, loadFromBitcastPointerAttachesPointeeMetadata)
{
	parseInput(R"(
		@gv = global i32 0
		define float @func() {
			%p = bitcast i32* @gv to float*
			%a = load float, float* %p
			ret float %a
		}
	)");
	auto* ld = getNthInstruction<LoadInst>();

	bool ret = inst_opt::optimize(ld);
	EXPECT_TRUE(ret);

	auto* nl = getNthInstruction<LoadInst>();
	ASSERT_NE(nullptr, nl);
	auto* i32 = Type::getInt32Ty(context);
	EXPECT_EQ(i32, llvm_utils::getPointeeTypeMetadata(nl));
	EXPECT_EQ(i32, llvm_utils::pointeeType(nl->getPointerOperand()));
}

TEST_F(OptimizeTests, loadFromBitcastPointerIntToPtrAttachesPointeeMetadata)
{
	parseInput(R"(
		@gv = global i32 0
		define i8 @func() {
			%p = bitcast i32* @gv to i8**
			%a = load i8*, i8** %p
			%v = load i8, i8* %a
			ret i8 %v
		}
	)");
	auto* ld = getNthInstruction<LoadInst>();

	bool ret = inst_opt::optimize(ld);
	EXPECT_TRUE(ret);

	auto* i2p = getNthInstruction<IntToPtrInst>();
	ASSERT_NE(nullptr, i2p);
	auto* i8 = Type::getInt8Ty(context);
	EXPECT_EQ(i8, llvm_utils::getPointeeTypeMetadata(i2p));
	EXPECT_EQ(i8, llvm_utils::pointeeType(i2p));
}

TEST_F(OptimizeTests, castSequence_float_int_float_arg)
{
	parseInput(R"(
		declare void @print (float)
		define void @func(float %a) {
			%b = bitcast float %a to i32
			%c = bitcast i32 %b to float
			call void @print(float %c)
			ret void
		}
	)");
	auto* i = getInstructionByName("c");

	bool ret = inst_opt::optimize(i);

	std::string exp = R"(
		declare void @print (float)
		define void @func(float %a) {
			call void @print(float %a)
			ret void
		}
	)";
	checkModuleAgainstExpectedIr(exp);
	EXPECT_TRUE(ret);
}

TEST_F(OptimizeTests, castSequence_float_int_float_local)
{
	parseInput(R"(
		declare void @print (float*)
		define void @func() {
			%a = alloca float
			%b = bitcast float* %a to i32*
			%c = bitcast i32* %b to float*
			call void @print(float* %c)
			ret void
		}
	)");
	auto* i = getInstructionByName("c");

	bool ret = inst_opt::optimize(i);

	std::string exp = R"(
		declare void @print (float*)
		define void @func() {
			%a = alloca float
			call void @print(float* %a)
			ret void
		}
	)";
	checkModuleAgainstExpectedIr(exp);
	EXPECT_TRUE(ret);
}

TEST_F(OptimizeTests, castSequence_ptr_int_ptr_global)
{
	parseInput(R"(
		@gv = global float 0.000000e+00
		declare void @print (float*)
		define void @func() {
			%a = bitcast float* @gv to i32*
			%b = ptrtoint i32* %a to i32
			%c = inttoptr i32 %b to i32*
			%d = bitcast i32* %c to float*
			call void @print(float* %d)
			ret void
		}
	)");
	auto* b = getInstructionByName("b");
	auto* c = getInstructionByName("c");
	auto* d = getInstructionByName("d");

	bool ret = inst_opt::optimize(b);
	ret |= inst_opt::optimize(c);
	ret |= inst_opt::optimize(d);

	std::string exp = R"(
		@gv = global float 0.000000e+00
		declare void @print (float*)
		define void @func() {
			call void @print(float* @gv)
			ret void
		}
	)";
	checkModuleAgainstExpectedIr(exp);
}

TEST_F(OptimizeTests, castSequence_float_int_flot_global)
{
	parseInput(R"(
		@gv = global float 0.000000e+00
		declare void @print (float)
		define void @func() {
			%a = load float, float* @gv
			%b = bitcast float %a to i32
			%c = bitcast i32 %b to float
			call void @print(float %c)
			ret void
		}
	)");
	auto* i = getInstructionByName("c");

	bool ret = inst_opt::optimize(i);

	std::string exp = R"(
		@gv = global float 0.000000e+00
		declare void @print (float)
		define void @func() {
			%a = load float, float* @gv
			call void @print(float %a)
			ret void
		}
	)";
	checkModuleAgainstExpectedIr(exp);
	EXPECT_TRUE(ret);
}

//
// Two loads of one pointer are two values as soon as anything writes
//

TEST_F(OptimizeTests, xorLoadLoadWithStoreBetweenIsNotZero)
{
	parseInput(R"(
		@g = global i32 0
		define i32 @fnc(i32 %x) {
			store i32 %x, i32* @g
			%a = load i32, i32* @g
			store i32 7, i32* @g
			%b = load i32, i32* @g
			%c = xor i32 %a, %b
			ret i32 %c
		}
	)");
	auto* i = getInstructionByName("c");

	bool ret = inst_opt::optimize(i);

	std::string exp = R"(
		@g = global i32 0
		define i32 @fnc(i32 %x) {
			store i32 %x, i32* @g
			%a = load i32, i32* @g
			store i32 7, i32* @g
			%b = load i32, i32* @g
			%c = xor i32 %a, %b
			ret i32 %c
		}
	)";
	checkModuleAgainstExpectedIr(exp);
	EXPECT_FALSE(ret);
}

TEST_F(OptimizeTests, orLoadLoadWithStoreBetweenIsNotOneOfThem)
{
	parseInput(R"(
		@g = global i32 0
		define i32 @fnc(i32 %x) {
			store i32 %x, i32* @g
			%a = load i32, i32* @g
			store i32 7, i32* @g
			%b = load i32, i32* @g
			%c = or i32 %a, %b
			ret i32 %c
		}
	)");
	auto* i = getInstructionByName("c");

	bool ret = inst_opt::optimize(i);

	std::string exp = R"(
		@g = global i32 0
		define i32 @fnc(i32 %x) {
			store i32 %x, i32* @g
			%a = load i32, i32* @g
			store i32 7, i32* @g
			%b = load i32, i32* @g
			%c = or i32 %a, %b
			ret i32 %c
		}
	)";
	checkModuleAgainstExpectedIr(exp);
	EXPECT_FALSE(ret);
}

TEST_F(OptimizeTests, andLoadLoadWithCallBetweenIsNotOneOfThem)
{
	parseInput(R"(
		@g = global i32 0
		declare void @f()
		define i32 @fnc() {
			%a = load i32, i32* @g
			call void @f()
			%b = load i32, i32* @g
			%c = and i32 %a, %b
			ret i32 %c
		}
	)");
	auto* i = getInstructionByName("c");

	bool ret = inst_opt::optimize(i);

	EXPECT_FALSE(ret);
}

// The fold itself is sound for one volatile load used twice -- `xor a, a` is
// zero whatever a is -- but the read must still happen.
TEST_F(OptimizeTests, xorVolatileLoadWithItselfKeepsTheLoad)
{
	parseInput(R"(
		@g = global i32 0
		define i32 @fnc() {
			%a = load volatile i32, i32* @g
			%b = xor i32 %a, %a
			ret i32 %b
		}
	)");
	auto* i = getInstructionByName("b");

	bool ret = inst_opt::optimize(i);

	std::string exp = R"(
		@g = global i32 0
		define i32 @fnc() {
			%a = load volatile i32, i32* @g
			ret i32 0
		}
	)";
	checkModuleAgainstExpectedIr(exp);
	EXPECT_TRUE(ret);
}

//
// Reassociating two adds does not carry their wrap flags
//

TEST_F(OptimizeTests, addSequenceDropsWrapFlags)
{
	parseInput(R"(
		define i8 @fnc(i8 %x) {
			%a = add nsw i8 %x, 127
			%b = add nsw i8 %a, 127
			ret i8 %b
		}
	)");
	auto* i = getInstructionByName("b");

	bool ret = inst_opt::optimize(i);

	std::string exp = R"(
		define i8 @fnc(i8 %x) {
			%b = add i8 %x, -2
			ret i8 %b
		}
	)";
	checkModuleAgainstExpectedIr(exp);
	EXPECT_TRUE(ret);
}

//
// A cast chain may only be collapsed if nothing in it narrowed
//

TEST_F(OptimizeTests, castSequenceKeepsADoubleFloatDoubleRoundTrip)
{
	parseInput(R"(
		define double @fnc(double %x) {
			%a = fptrunc double %x to float
			%b = fpext float %a to double
			ret double %b
		}
	)");
	auto* i = getInstructionByName("b");

	bool ret = inst_opt::optimize(i);

	std::string exp = R"(
		define double @fnc(double %x) {
			%a = fptrunc double %x to float
			%b = fpext float %a to double
			ret double %b
		}
	)";
	checkModuleAgainstExpectedIr(exp);
	EXPECT_FALSE(ret);
}

// Widening and narrowing back is the identity, so this one still collapses.
TEST_F(OptimizeTests, castSequenceCollapsesAFloatDoubleFloatRoundTrip)
{
	parseInput(R"(
		define float @fnc(float %x) {
			%a = fpext float %x to double
			%b = fptrunc double %a to float
			ret float %b
		}
	)");
	auto* i = getInstructionByName("b");

	bool ret = inst_opt::optimize(i);

	std::string exp = R"(
		define float @fnc(float %x) {
			ret float %x
		}
	)";
	checkModuleAgainstExpectedIr(exp);
	EXPECT_TRUE(ret);
}

// The test harness declares 32-bit pointers, so a chain through i16 narrows.
TEST_F(OptimizeTests, castSequenceKeepsANarrowingPointerChain)
{
	parseInput(R"(
		declare void @print(i8*)
		define void @func(i32* %p) {
			%a = ptrtoint i32* %p to i32
			%b = trunc i32 %a to i16
			%c = inttoptr i16 %b to i8*
			call void @print(i8* %c)
			ret void
		}
	)");
	auto* i = getInstructionByName("c");

	bool ret = inst_opt::optimize(i);

	EXPECT_FALSE(ret);
}

//
// Rebuilding a store must not quietly make it non-volatile
//

TEST_F(OptimizeTests, storeToBitcastPointerLeavesAVolatileStoreAlone)
{
	parseInput(R"(
		@gv = global i32 0
		define void @fnc(float %val) {
			store volatile float %val, float* bitcast (i32* @gv to float*)
			ret void
		}
	)");
	auto* i = getNthInstruction<StoreInst>();

	bool ret = inst_opt::optimize(i);

	EXPECT_FALSE(ret);
}

TEST_F(OptimizeTests, loadFromBitcastPointerLeavesAVolatileLoadAlone)
{
	parseInput(R"(
		@gv = global i32 0
		define float @fnc() {
			%a = load volatile float, float* bitcast (i32* @gv to float*)
			ret float %a
		}
	)");
	auto* i = getInstructionByName("a");

	bool ret = inst_opt::optimize(i);

	EXPECT_FALSE(ret);
}

} // namespace tests
} // namespace bin2llvmir
} // namespace retdec
