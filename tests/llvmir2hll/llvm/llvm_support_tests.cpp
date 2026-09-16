/**
* @file tests/llvmir2hll/llvm/llvm_support_tests.cpp
* @brief Tests for the @c llvm_support module.
* @copyright (c) 2017 Avast Software, licensed under the MIT license
* @copyright (c) 2025-2026 Odin Loch trading as Imortek (modifications)
*/

#include <memory>
#include <string>

#include <gtest/gtest.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>

#include "retdec/llvmir2hll/llvm/llvm_support.h"

using namespace ::testing;

namespace retdec {
namespace llvmir2hll {
namespace tests {

/**
* @brief Tests for the @c llvm_support module.
*/
class LLVMSupportTests: public Test {};

//
// isBasicBlockLabel()
//

TEST_F(LLVMSupportTests,
IsLLVMBasicBlockLabelIsLabel) {
	EXPECT_TRUE(LLVMSupport::isBasicBlockLabel(
		LLVMSupport::getBasicBlockLabelPrefix() + "8903087"));
	EXPECT_TRUE(LLVMSupport::isBasicBlockLabel(
		LLVMSupport::getBasicBlockLabelPrefix() + "8900368"));
	EXPECT_TRUE(LLVMSupport::isBasicBlockLabel(
		LLVMSupport::getBasicBlockLabelPrefix() + "4534"));
	EXPECT_TRUE(LLVMSupport::isBasicBlockLabel(
		LLVMSupport::getBasicBlockLabelPrefix() + "1"));
}

TEST_F(LLVMSupportTests,
IsLLVMBasicBlockLabelIsNotLabel) {
	EXPECT_FALSE(LLVMSupport::isBasicBlockLabel(
		""));
	EXPECT_FALSE(LLVMSupport::isBasicBlockLabel(
		LLVMSupport::getBasicBlockLabelPrefix()));
	EXPECT_FALSE(LLVMSupport::isBasicBlockLabel(
		"pc_8900368"));
	EXPECT_FALSE(LLVMSupport::isBasicBlockLabel(
		"dream away"));
	EXPECT_FALSE(LLVMSupport::isBasicBlockLabel(
		LLVMSupport::getBasicBlockLabelPrefix() + "fgxxx"));
}

//
// isInlinableInst()
//
// An instruction that is inlinable is emitted at its use site as an
// expression, by LLVMInstructionConverter. An instruction that is not is
// emitted where it stands, as a statement, by BasicBlockConverter, and its
// uses refer to a variable.
//
// atomicrmw and cmpxchg have to be the second kind, for the reason LoadInst
// is: each one is a load AND a store, and moving it to its use site reorders
// a write. They were the first kind, and since LLVMInstructionConverter has
// no case for either, what came out was not a wrong expression but
// visitInstruction() and an abort -- reached by any binary whose ARM64 LSE
// atomics write their old value into a register.
//

namespace {

llvm::Function* makeTestFunction(llvm::Module& m, llvm::Type* retTy)
{
	auto& ctx = m.getContext();
	auto* fty = llvm::FunctionType::get(retTy, {llvm::PointerType::getUnqual(ctx), llvm::Type::getInt32Ty(ctx)}, false);
	auto* f = llvm::Function::Create(fty, llvm::GlobalValue::ExternalLinkage, "f", &m);
	llvm::BasicBlock::Create(ctx, "entry", f);
	return f;
}

} // anonymous namespace

TEST_F(LLVMSupportTests, AtomicRMWIsNotInlinable)
{
	llvm::LLVMContext ctx;
	auto module = std::make_unique<llvm::Module>("m", ctx);
	auto* f = makeTestFunction(*module, llvm::Type::getInt64Ty(ctx));
	llvm::IRBuilder<> irb(&f->getEntryBlock());

	auto* rmw = irb.CreateAtomicRMW(
		llvm::AtomicRMWInst::Add, f->getArg(0), f->getArg(1), llvm::MaybeAlign(), llvm::AtomicOrdering::AcquireRelease);
	irb.CreateRet(irb.CreateZExt(rmw, llvm::Type::getInt64Ty(ctx)));

	// Exactly one use, in the same basic block, not a call, load, phi or
	// terminator: every condition the list looked at said "inline me".
	EXPECT_TRUE(rmw->hasOneUse());
	EXPECT_FALSE(LLVMSupport::isInlinableInst(rmw));
}

TEST_F(LLVMSupportTests, AtomicCmpXchgIsNotInlinable)
{
	llvm::LLVMContext ctx;
	auto module = std::make_unique<llvm::Module>("m", ctx);
	auto* f = makeTestFunction(*module, llvm::Type::getInt32Ty(ctx));
	llvm::IRBuilder<> irb(&f->getEntryBlock());

	auto* cx = irb.CreateAtomicCmpXchg(
		f->getArg(0),
		f->getArg(1),
		f->getArg(1),
		llvm::MaybeAlign(),
		llvm::AtomicOrdering::Acquire,
		llvm::AtomicOrdering::Acquire);
	irb.CreateRet(irb.CreateExtractValue(cx, 0));

	EXPECT_TRUE(cx->hasOneUse());
	EXPECT_FALSE(LLVMSupport::isInlinableInst(cx));
}

TEST_F(LLVMSupportTests, PlainArithmeticIsStillInlinable)
{
	// The other side of the same question: excluding the atomics must not
	// exclude everything, or the two tests above would pass against a
	// function that always answers false.
	llvm::LLVMContext ctx;
	auto module = std::make_unique<llvm::Module>("m", ctx);
	auto* f = makeTestFunction(*module, llvm::Type::getInt32Ty(ctx));
	llvm::IRBuilder<> irb(&f->getEntryBlock());

	auto* add = irb.CreateAdd(f->getArg(1), f->getArg(1));
	irb.CreateRet(irb.CreateMul(add, f->getArg(1)));

	EXPECT_TRUE(LLVMSupport::isInlinableInst(llvm::cast<llvm::Instruction>(add)));
}

} // namespace tests
} // namespace llvmir2hll
} // namespace retdec
