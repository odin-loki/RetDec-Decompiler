/**
 * @file include/retdec/bin2llvmir/optimizations/idioms/idioms_abstract.h
 * @brief Instruction idioms analysis abstract class
 * @copyright (c) 2017 Avast Software, licensed under the MIT license
 * @copyright (c) 2025-2026 Odin Loch trading as Imortek (modifications)
 */

#ifndef RETDEC_BIN2LLVMIR_OPTIMIZATIONS_IDIOMS_IDIOMS_ABSTRACT_H
#define RETDEC_BIN2LLVMIR_OPTIMIZATIONS_IDIOMS_IDIOMS_ABSTRACT_H

#include <cstdint>
#include <vector>

#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/Instruction.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/ValueHandle.h>
#include <llvm/Pass.h>

#include "retdec/bin2llvmir/optimizations/idioms/idioms_types.h"

namespace retdec {
namespace bin2llvmir {

/**
 * @brief Instruction idiom analysis abstract class
 */
class IdiomsAbstract {
private:
	CC_arch m_arch;
	CC_compiler m_compiler;
	llvm::Module * m_module;

	/// Values eraseInstFromBasicBlock could not erase yet.  Weak handles, not
	/// raw pointers: another rewrite in the same pass may erase one of these
	/// before the queue is drained.
	mutable std::vector<llvm::WeakTrackingVH> m_deferredErase;

protected:
	IdiomsAbstract();

	void init(llvm::Module * M, CC_compiler cc, CC_arch arch);

	CC_compiler getCompiler() const { return m_compiler; }
	CC_arch getArch() const { return m_arch; }
	llvm::Module * getModule() const { return m_module; }

	virtual bool doAnalysis(llvm::Function &, llvm::Pass *) = 0;
	virtual ~IdiomsAbstract() = default;

	bool findBranchDependingOn(llvm::BranchInst ** br, llvm::BasicBlock & bb,
		const llvm::Value * val) const;
	/// Remove @a val from @a bb once nothing reads it.
	///
	/// An idiom rewrite matches a small tree and calls this on the tree's
	/// inner nodes.  At the moment of the call those nodes are still read by
	/// the tree's root, which the driver has not replaced yet, so "does
	/// anything still use it" cannot be answered here.  It used to be answered
	/// by not asking: the value was replaced with `undef` and erased, which is
	/// correct when the root was its only reader and a miscompile when it was
	/// not -- an unrelated instruction silently starts reading `undef`.
	///
	/// So a value that still has users is queued instead, and
	/// drainDeferredErases() empties the queue after the driver has finished
	/// the rewrite.  By then a genuinely dead node has no users and goes; one
	/// that something else reads keeps its users and stays.
	void eraseInstFromBasicBlock(llvm::Value* val, llvm::BasicBlock* bb) const;

	/// Erase everything queued by eraseInstFromBasicBlock that is now dead.
	/// Runs to a fixpoint, because erasing one node can be what makes the next
	/// one dead.
	void drainDeferredErases() const;
	static bool isPowerOfTwo(unsigned x);
	static bool isUsableDivisor(int64_t divisor);
	static bool isPowerOfTwoRepresentable(const llvm::ConstantInt *cnst);
};

} // namespace bin2llvmir
} // namespace retdec

#endif
