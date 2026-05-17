/**
 * @file tests/bin2llvmir/optimizations/jump_table_recovery/jump_table_recovery_tests.cpp
 * @brief Tests for @c JumpTableRecovery.
 * @copyright (c) 2026, MIT license
 */

#include "bin2llvmir/utils/llvmir_tests.h"
#include "retdec/bin2llvmir/optimizations/jump_table_recovery/jump_table_recovery.h"
#include "retdec/bin2llvmir/providers/abi/x86.h"

using namespace ::testing;
using namespace llvm;

namespace retdec {
namespace bin2llvmir {
namespace tests {

class JumpTableRecoveryTests: public LlvmIrTests
{
protected:
	JumpTableRecovery pass;
};

TEST_F(JumpTableRecoveryTests, RecoversIndirectBrWithBlockAddressTable)
{
	parseInput(R"(
@jtbl = private unnamed_addr constant [2 x i8*] [i8* blockaddress(@dispatch_fn, %bb0), i8* blockaddress(@dispatch_fn, %bb1)]

define void @dispatch_fn(i32 %idx) {
entry:
  br label %dispatch
dispatch:
  %p = getelementptr inbounds [2 x i8*], [2 x i8*]* @jtbl, i32 0, i32 %idx
  %a = load i8*, i8** %p
  indirectbr i8* %a, [label %bb0, label %bb1]

bb0:
  ret void

bb1:
  ret void
}
	)");

	auto c = Config::empty(module.get());
	c.getConfig().architecture.setIsX86();
	AbiX86 abi(module.get(), &c);

	bool changed = pass.runOnModuleCustom(*module, &abi, &c, nullptr);
	EXPECT_TRUE(changed);
	EXPECT_EQ(nullptr, getNthInstruction<IndirectBrInst>());
	ASSERT_NE(nullptr, getNthInstruction<SwitchInst>());
}

TEST_F(JumpTableRecoveryTests, SkipsFullyConstantGep)
{
	parseInput(R"(
@jtbl = private unnamed_addr constant [2 x i8*] [i8* blockaddress(@f, %bb0), i8* blockaddress(@f, %bb1)]

define void @f() {
entry:
  %p = getelementptr inbounds [2 x i8*], [2 x i8*]* @jtbl, i32 0, i32 0
  %a = load i8*, i8** %p
  indirectbr i8* %a, [label %bb0, label %bb1]

bb0:
  ret void

bb1:
  ret void
}
	)");

	auto c = Config::empty(module.get());
	c.getConfig().architecture.setIsX86();
	AbiX86 abi(module.get(), &c);

	bool changed = pass.runOnModuleCustom(*module, &abi, &c, nullptr);
	EXPECT_FALSE(changed);
	EXPECT_NE(nullptr, getNthInstruction<IndirectBrInst>());
}

TEST_F(JumpTableRecoveryTests, RecoversIndirectBrWithIntegerTableAndInsnAddr)
{
	parseInput(R"(
@jtbl = private unnamed_addr constant [2 x i64] [i64 4096, i64 8192]

define void @dispatch_fn(i32 %idx) {
entry:
  br label %dispatch
dispatch:
  %p = getelementptr inbounds [2 x i64], [2 x i64]* @jtbl, i32 0, i32 %idx
  %a = load i64, i64* %p
  %ap = inttoptr i64 %a to i8*
  indirectbr i8* %ap, [label %bb0, label %bb1]

bb0:
  ret void, !insn.addr !0

bb1:
  ret void, !insn.addr !1
}

!0 = !{i64 4096}
!1 = !{i64 8192}
	)");

	auto c = Config::empty(module.get());
	c.getConfig().architecture.setIsX86();
	AbiX86 abi(module.get(), &c);

	bool changed = pass.runOnModuleCustom(*module, &abi, &c, nullptr);
	EXPECT_TRUE(changed);
	EXPECT_EQ(nullptr, getNthInstruction<IndirectBrInst>());
	ASSERT_NE(nullptr, getNthInstruction<SwitchInst>());
}

TEST_F(JumpTableRecoveryTests, RecoversIntegerTableWhenAddrNearInsnMetadata)
{
	// Table holds 4100; nearest !insn.addr is 4096 (within 64 bytes) → still maps.
	parseInput(R"(
@jtbl = private unnamed_addr constant [2 x i64] [i64 4100, i64 8192]

define void @dispatch_fn(i32 %idx) {
entry:
  br label %dispatch
dispatch:
  %p = getelementptr inbounds [2 x i64], [2 x i64]* @jtbl, i32 0, i32 %idx
  %a = load i64, i64* %p
  %ap = inttoptr i64 %a to i8*
  indirectbr i8* %ap, [label %bb0, label %bb1]

bb0:
  ret void, !insn.addr !0

bb1:
  ret void, !insn.addr !1
}

!0 = !{i64 4096}
!1 = !{i64 8192}
	)");

	auto c = Config::empty(module.get());
	c.getConfig().architecture.setIsX86();
	AbiX86 abi(module.get(), &c);

	bool changed = pass.runOnModuleCustom(*module, &abi, &c, nullptr);
	EXPECT_TRUE(changed);
	EXPECT_EQ(nullptr, getNthInstruction<IndirectBrInst>());
	ASSERT_NE(nullptr, getNthInstruction<SwitchInst>());
}

TEST_F(JumpTableRecoveryTests,
	DefaultSuccessorWinsWhenTwoPredsVoteForSameNonDispatchBlock)
{
	// Two conditional predecessors of %dispatch each use a different edge order
	// (dispatch as false vs true successor) but both bypass to the same block.
	parseInput(R"(
@jtbl = private unnamed_addr constant [2 x i8*] [i8* blockaddress(@dispatch_fn, %bb0), i8* blockaddress(@dispatch_fn, %bb1)]

define void @dispatch_fn(i32 %idx, i1 %pick) {
entry:
  br i1 %pick, label %pred1, label %pred2

pred1:
  %c1 = icmp eq i32 %idx, -1
  br i1 %c1, label %sw_default, label %dispatch

pred2:
  %c2 = icmp eq i32 %idx, -2
  br i1 %c2, label %dispatch, label %sw_default

dispatch:
  %p = getelementptr inbounds [2 x i8*], [2 x i8*]* @jtbl, i32 0, i32 %idx
  %a = load i8*, i8** %p
  indirectbr i8* %a, [label %bb0, label %bb1]

bb0:
  ret void

bb1:
  ret void

sw_default:
  ret void
}
	)");

	auto c = Config::empty(module.get());
	c.getConfig().architecture.setIsX86();
	AbiX86 abi(module.get(), &c);

	bool changed = pass.runOnModuleCustom(*module, &abi, &c, nullptr);
	EXPECT_TRUE(changed);
	auto* sw = getNthInstruction<SwitchInst>();
	ASSERT_NE(nullptr, sw);
	BasicBlock* expectedDefault = nullptr;
	for (auto& bb : *getFunctionByName("dispatch_fn"))
	{
		if (bb.getName() == "sw_default")
		{
			expectedDefault = &bb;
			break;
		}
	}
	ASSERT_NE(nullptr, expectedDefault);
	EXPECT_EQ(expectedDefault, sw->getDefaultDest());
}

} // namespace tests
} // namespace bin2llvmir
} // namespace retdec
