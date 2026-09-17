/**
 * @file src/bin2llvmir/optimizations/idioms/idioms_abstract.cpp
 * @brief Implementation of the instruction idioms analysis abstract class
 * @copyright (c) 2017 Avast Software, licensed under the MIT license
 * @copyright (c) 2025-2026 Odin Loch trading as Imortek (modifications)
 */

#include "retdec/bin2llvmir/optimizations/idioms/idioms_abstract.h"

using namespace llvm;

namespace retdec {
namespace bin2llvmir {

IdiomsAbstract::IdiomsAbstract():
	m_arch(ARCH_ANY), m_compiler(CC_ANY), m_module(nullptr) {}

void IdiomsAbstract::init(llvm::Module * M, CC_compiler cc, CC_arch arch) {
	m_compiler = cc;
	m_arch = arch;
	m_module = M;
}

/**
 * Find a branch instruction in a BasicBlock
 *
 * @param br found branch instruction
 * @param bb basic block to look for br
 * @param val Value or Instruction with branch use
 */
bool IdiomsAbstract::findBranchDependingOn(llvm::BranchInst ** br, llvm::BasicBlock & bb,
		const llvm::Value * val) const {
	for (llvm::BasicBlock::iterator i = bb.begin(); i != bb.end(); ++i) {
		if ((*br = llvm::dyn_cast<llvm::BranchInst>(i)))
			if ((*br)->getNumOperands() >= 1 && (*br)->getOperand(0) == val)
				return true;
	}

	*br = nullptr;
	return false;
}

/**
 * Look for instruction by value and erase it from module.
 *
 * @param val instruction value to look for
 * @param bb BasicBlock to erase instruction from
 */
void IdiomsAbstract::eraseInstFromBasicBlock(llvm::Value* val, llvm::BasicBlock* bb) const
{
	for (llvm::BasicBlock::iterator end = bb->end(), i = bb->begin(); i != end; ++i) {
		llvm::Value * rem = static_cast<llvm::Value*>(&(*i));
		if (val != rem)
		{
			continue;
		}

		// This used to be replaceAllUsesWith(UndefValue) followed by an erase,
		// which erases unconditionally and makes every remaining reader of the
		// value read `undef`.  That is right only when the idiom's own root
		// was the value's sole reader.  It is not always:
		//
		//     %y = lshr i32 %x, 31
		//     %z = xor  i32 %y, 1     <- ((X u>> 31) ^ 1), rewritten to X >= 0
		//     %w = add  i32 %y, 5     <- nothing to do with the idiom
		//
		// exchangeGreaterEqualZero fires on %z and erases %y, and %w becomes
		// `add i32 undef, 5`.  Measured, not reasoned about: see IDIOM-USE-01.
		//
		// The root is still live at this point -- the driver replaces it after
		// the exchanger returns -- so "is this dead" cannot be decided here.
		// Queue it and let drainDeferredErases() decide once the rewrite is
		// complete.
		if (!rem->use_empty())
		{
			m_deferredErase.push_back(llvm::WeakTrackingVH(rem));
			return;
		}

		(*i).eraseFromParent();
		return;
	}
}

void IdiomsAbstract::drainDeferredErases() const
{
	// Erasing one queued value can be what makes the next one dead, and the
	// queue is in no particular order, so this repeats until a pass over the
	// whole queue erases nothing.
	bool progress = true;
	while (progress)
	{
		progress = false;
		for (auto& handle: m_deferredErase)
		{
			llvm::Value* v = handle;
			if (!v)
			{
				continue; // erased by some other rewrite already
			}
			auto* inst = llvm::dyn_cast<llvm::Instruction>(v);
			if (!inst || !inst->use_empty() || inst->isTerminator())
			{
				continue;
			}
			if (inst->mayHaveSideEffects())
			{
				continue; // dead by use count is not dead by effect
			}
			inst->eraseFromParent();
			progress = true;
		}
	}
	m_deferredErase.clear();
}

/**
 * Is a divisor recovered from an idiom safe to put in the module?
 *
 * No. Not unless it is at least two in magnitude.
 *
 * Zero is the one that matters: `sdiv i32 %x, 0` is not a poison value the
 * optimiser has to carry around, it is immediate undefined behaviour, which
 * licenses it to delete whatever follows. And the magic-number helpers reach
 * zero easily. Calling divisorByMagicNumberSigned2 over magic < 4096 and
 * shift <= 40 -- 167936 pairs -- answers 0 on 36869 of them, because the
 * `q == 0` check inside it does not bound the value that comes out: the ceil
 * step `++result` on a uint32_t wraps 0xFFFFFFFF to 0, and the signed helpers
 * divide by an INT32_MIN quotient and truncate to 0.
 *
 * One is not undefined, but it is still wrong: no compiler emits a magic
 * multiply to divide by one, so a divisor of one means the pattern matched
 * something that was never a division. Same for minus one, which additionally
 * makes `sdiv INT_MIN, -1` -- undefined behaviour again.
 *
 * So the recovered value has to be a divisor a compiler would actually have
 * used a magic number for, and that starts at two.
 */
bool IdiomsAbstract::isUsableDivisor(int64_t divisor)
{
	return divisor <= -2 || divisor >= 2;
}

/**
 * Is value a power of two?
 * @param x value to be check
 * @return true if value is power of two or zero
 */
bool IdiomsAbstract::isPowerOfTwo(unsigned x) {
	return ((x != 0) && ! (x & (x - 1)));
}

/**
* Is @c 2^cnst representable on the bit width of @a cnst?
*
* If you want to compute <tt>pow(2, cnst)</tt>, always ensure that @c
* isPowerOfTwoRepresentable(cnst) returns @c true.
*/
bool IdiomsAbstract::isPowerOfTwoRepresentable(const ConstantInt *cnst) {
	// When cnst has bit width X (i.e. its type is iX), representable powers
	// are up to 2^(X - 2). For example, when the type of cnst is i32,
	// representable powers are up to 2^30. 2^31 is not representable because
	// 2^31 == 2147483648, which is not representable on 32 bits when
	// considered the type to be a signed integer. Technically, it is
	// representable on 32 bits as an unsigned integer, but when considered as
	// a signed value, it is not representable.
	//
	// Consider an optimization of the following idiom:
	//
	//     %b = shl i32 %a, 31
	//
	// If we allowed 2^31 to be representable, we would end up with the
	// following replacement of the original instruction:
	//
	//     %b = mul i32 %a, -2147483648
	//
	// However, this causes -instcombine to loop/crash, which is the
	// reason why we consider 2^31 to be unrepresentable. Testing example:
	//
	//     define i32 @func(i32 %a) {
	//       %b = add i32 %a, 1
	//       %c = mul i32 %b, -2147483648
	//       ret i32 %c
	//     }
	//
	// When the above example was compiled and run, opt failed to produce a
	// result:
	//
	//     ./llvm-as func.ll && ./opt -instcombine -o func-opt.bc < func.bc
	//
	// By not allowing such optimizations, we ensure that opt does not fail.
	// And this is precisely what this function does.
	return cnst->getZExtValue() < (cnst->getBitWidth() - 1);
}

} // namespace bin2llvmir
} // namespace retdec
