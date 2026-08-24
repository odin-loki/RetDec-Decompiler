/**
 * @file tests/retdec/llvm_to_ssa_test.cpp
 * @brief E6: llvm_to_ssa on a compiled-style for-loop.
 */

#include "llvm_to_ssa.h"

#include "retdec/ssa/ssa.h"

#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <llvm/IRReader/IRReader.h>
#include <llvm/Support/MemoryBuffer.h>
#include <llvm/Support/SourceMgr.h>

#include <gtest/gtest.h>

#include <memory>
#include <string>

using retdec::buildSsaModule;
using retdec::ssa::IrInstr;
using retdec::ssa::SSAFunction;
using retdec::ssa::ValueKind;

namespace {

// Same shape clang emits for `int sum(int *p, int n) { int a = 0; for (int i = 0;
// i < n; ++i) a += p[i]; return a; }` at -O1 (typed pointers, LLVM 8).
constexpr const char* kForLoopIR = R"IR(
target datalayout = "e-m:e-i64:64-f80:128-n8:16:32:64-S128"
define i32 @sum_loop(i32* %p, i32 %n) {
entry:
  br label %header
header:
  %i = phi i32 [ 0, %entry ], [ %i.next, %latch ]
  %acc = phi i32 [ 0, %entry ], [ %acc.next, %latch ]
  %cmp = icmp slt i32 %i, %n
  br i1 %cmp, label %body, label %exit
body:
  %elem.ptr = getelementptr i32, i32* %p, i32 %i
  %elem = load i32, i32* %elem.ptr
  %acc.next = add i32 %acc, %elem
  br label %latch
latch:
  %i.next = add i32 %i, 1
  br label %header
exit:
  ret i32 %acc
}
)IR";

std::unique_ptr<llvm::Module> parseIR(llvm::LLVMContext& ctx, const char* ir)
{
	auto mb = llvm::MemoryBuffer::getMemBuffer(ir);
	llvm::SMDiagnostic err;
	auto module = llvm::parseIR(mb->getMemBufferRef(), err, ctx);
	if (!module)
	{
		ADD_FAILURE() << err.getMessage().str();
	}
	return module;
}

const SSAFunction* findFn(const retdec::ssa::SSAModule& mod, const std::string& name)
{
	for (const auto& fn: mod.functions)
	{
		if (fn && fn->name() == name) return fn.get();
	}
	return nullptr;
}

bool hasBackEdge(const SSAFunction& fn)
{
	for (uint32_t b = 0; b < fn.blockCount(); ++b)
	{
		const auto* blk = fn.block(b);
		if (!blk) continue;
		for (uint32_t s: blk->succs)
			if (s <= b) return true;
	}
	return false;
}

int countOp(const SSAFunction& fn, IrInstr::Op op)
{
	int n = 0;
	for (uint32_t b = 0; b < fn.blockCount(); ++b)
	{
		const auto* blk = fn.block(b);
		if (!blk) continue;
		for (const auto* i: blk->instrs)
			if (i && i->op == op) ++n;
	}
	return n;
}

bool hasAddImmediate(const SSAFunction& fn, uint64_t imm)
{
	for (uint32_t b = 0; b < fn.blockCount(); ++b)
	{
		const auto* blk = fn.block(b);
		if (!blk) continue;
		for (const auto* i: blk->instrs)
		{
			if (!i || i->op != IrInstr::Op::Add) continue;
			for (const auto& u: i->uses)
			{
				const auto* v = fn.value(u.valueId);
				if (v && v->kind == ValueKind::Immediate && v->imm == imm) return true;
			}
		}
	}
	return false;
}

bool hasPhiImmediate(const SSAFunction& fn, uint64_t imm)
{
	for (uint32_t b = 0; b < fn.blockCount(); ++b)
	{
		const auto* blk = fn.block(b);
		if (!blk) continue;
		for (const auto* i: blk->instrs)
		{
			if (!i || i->op != IrInstr::Op::Phi) continue;
			for (const auto& u: i->uses)
			{
				const auto* v = fn.value(u.valueId);
				if (v && v->kind == ValueKind::Immediate && v->imm == imm) return true;
			}
		}
	}
	return false;
}

int phiNodeListCount(const SSAFunction& fn)
{
	int n = 0;
	for (uint32_t b = 0; b < fn.blockCount(); ++b)
	{
		const auto* blk = fn.block(b);
		if (blk) n += static_cast<int>(blk->phis.size());
	}
	return n;
}

} // namespace

TEST(LlvmToSsa, ForLoopHasBackEdgeHeaderPhiAndImmediateUses)
{
	llvm::LLVMContext ctx;
	auto module = parseIR(ctx, kForLoopIR);
	ASSERT_NE(module, nullptr);

	auto ssa = buildSsaModule(*module);
	ASSERT_NE(ssa, nullptr);
	const SSAFunction* fn = findFn(*ssa, "sum_loop");
	ASSERT_NE(fn, nullptr);

	EXPECT_TRUE(hasBackEdge(*fn));
	EXPECT_GE(countOp(*fn, IrInstr::Op::Phi), 2);
	EXPECT_TRUE(hasAddImmediate(*fn, 1));
	EXPECT_TRUE(hasPhiImmediate(*fn, 0));
	// SSAPass owns BasicBlock::phis. Filling it here would make
	// AccumulateDetector fire on every compiled loop.
	EXPECT_EQ(phiNodeListCount(*fn), 0);
}

constexpr const char* kRemLockIR = R"IR(
target datalayout = "e-m:e-i64:64-f80:128-n8:16:32:64-S128"
define i32 @wrap(i32 %a, i32 %n) {
  %r = srem i32 %a, %n
  ret i32 %r
}
define i32 @lockadd(i32* %p) {
  %r = atomicrmw add i32* %p, i32 1 seq_cst
  ret i32 %r
}
)IR";

constexpr const char* kAtomicLoadIR = R"IR(
target datalayout = "e-m:e-i64:64-f80:128-n8:16:32:64-S128"
define i32 @ldxr(i32* %p) {
  %r = load atomic i32, i32* %p monotonic, align 4
  ret i32 %r
}
)IR";

constexpr const char* kCmpXchgIR = R"IR(
target datalayout = "e-m:e-i64:64-f80:128-n8:16:32:64-S128"
define i32 @cas(i32* %p) {
  %r = cmpxchg i32* %p, i32 0, i32 1 seq_cst seq_cst
  %v = extractvalue { i32, i1 } %r, 0
  ret i32 %v
}
)IR";

TEST(LlvmToSsa, SRemMapsToRemNotDiv)
{
	llvm::LLVMContext ctx;
	auto module = parseIR(ctx, kRemLockIR);
	ASSERT_NE(module, nullptr);
	auto ssa = buildSsaModule(*module);
	ASSERT_NE(ssa, nullptr);
	const SSAFunction* fn = findFn(*ssa, "wrap");
	ASSERT_NE(fn, nullptr);
	EXPECT_GE(countOp(*fn, IrInstr::Op::Rem), 1);
	EXPECT_EQ(countOp(*fn, IrInstr::Op::Div), 0);
}

TEST(LlvmToSsa, AtomicRmwMapsToLock)
{
	llvm::LLVMContext ctx;
	auto module = parseIR(ctx, kRemLockIR);
	ASSERT_NE(module, nullptr);
	auto ssa = buildSsaModule(*module);
	ASSERT_NE(ssa, nullptr);
	const SSAFunction* fn = findFn(*ssa, "lockadd");
	ASSERT_NE(fn, nullptr);
	EXPECT_GE(countOp(*fn, IrInstr::Op::Lock), 1);
}

TEST(LlvmToSsa, AtomicLoadMapsToLock)
{
	llvm::LLVMContext ctx;
	auto module = parseIR(ctx, kAtomicLoadIR);
	ASSERT_NE(module, nullptr);
	auto ssa = buildSsaModule(*module);
	ASSERT_NE(ssa, nullptr);
	const SSAFunction* fn = findFn(*ssa, "ldxr");
	ASSERT_NE(fn, nullptr);
	EXPECT_GE(countOp(*fn, IrInstr::Op::Lock), 1);
	EXPECT_EQ(countOp(*fn, IrInstr::Op::Load), 0);
}

TEST(LlvmToSsa, CmpXchgMapsToLock)
{
	llvm::LLVMContext ctx;
	auto module = parseIR(ctx, kCmpXchgIR);
	ASSERT_NE(module, nullptr);
	auto ssa = buildSsaModule(*module);
	ASSERT_NE(ssa, nullptr);
	const SSAFunction* fn = findFn(*ssa, "cas");
	ASSERT_NE(fn, nullptr);
	EXPECT_GE(countOp(*fn, IrInstr::Op::Lock), 1);
}

constexpr const char* kInsnAddrIR = R"IR(
target datalayout = "e-m:e-i64:64-f80:128-n8:16:32:64-S128"
define i32 @addr(i32 %a, i32 %b) {
  %r = add i32 %a, %b, !insn.addr !0
  ret i32 %r
}
!0 = !{i64 4096}
)IR";

constexpr const char* kRetdecAddrIR = R"IR(
target datalayout = "e-m:e-i64:64-f80:128-n8:16:32:64-S128"
define i32 @legacy(i32 %a, i32 %b) {
  %r = add i32 %a, %b, !retdec.addr !0
  ret i32 %r
}
!0 = !{i64 8192}
)IR";

uint64_t firstOpVma(const SSAFunction& fn, IrInstr::Op op)
{
	for (uint32_t b = 0; b < fn.blockCount(); ++b)
	{
		const auto* blk = fn.block(b);
		if (!blk) continue;
		for (const auto* i: blk->instrs)
			if (i && i->op == op) return i->vma;
	}
	return 0;
}

TEST(LlvmToSsa, InsnAddrMetadataSetsVma)
{
	llvm::LLVMContext ctx;
	auto module = parseIR(ctx, kInsnAddrIR);
	ASSERT_NE(module, nullptr);
	auto ssa = buildSsaModule(*module);
	ASSERT_NE(ssa, nullptr);
	const SSAFunction* fn = findFn(*ssa, "addr");
	ASSERT_NE(fn, nullptr);
	EXPECT_EQ(firstOpVma(*fn, IrInstr::Op::Add), 4096u);
}

TEST(LlvmToSsa, RetdecAddrMetadataStillSetsVma)
{
	llvm::LLVMContext ctx;
	auto module = parseIR(ctx, kRetdecAddrIR);
	ASSERT_NE(module, nullptr);
	auto ssa = buildSsaModule(*module);
	ASSERT_NE(ssa, nullptr);
	const SSAFunction* fn = findFn(*ssa, "legacy");
	ASSERT_NE(fn, nullptr);
	EXPECT_EQ(firstOpVma(*fn, IrInstr::Op::Add), 8192u);
}
