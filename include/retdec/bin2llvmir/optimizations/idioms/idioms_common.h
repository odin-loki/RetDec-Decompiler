/**
 * @file include/retdec/bin2llvmir/optimizations/idioms/idioms_common.h
 * @brief Common compiler instruction idioms
 * @copyright (c) 2017 Odin Loch Trading as Imortek
 */

#ifndef RETDEC_BIN2LLVMIR_OPTIMIZATIONS_IDIOMS_IDIOMS_COMMON_H
#define RETDEC_BIN2LLVMIR_OPTIMIZATIONS_IDIOMS_IDIOMS_COMMON_H

#include "retdec/bin2llvmir/optimizations/idioms/idioms_abstract.h"

namespace retdec {
namespace bin2llvmir {

/**
 * @brief Common compiler instruction idioms
 */
class IdiomsCommon: virtual public IdiomsAbstract {
	friend class IdiomsAnalysis;
protected:
	llvm::Instruction * exchangeDivByMinusTwo(llvm::BasicBlock::iterator iter) const;
	llvm::Instruction * exchangeUnsignedModulo2n(llvm::BasicBlock::iterator iter) const;
	llvm::Instruction * exchangeLessThanZero(llvm::BasicBlock::iterator iter) const;
	llvm::Instruction * exchangeGreaterEqualZero(llvm::BasicBlock::iterator iter) const;
	llvm::Instruction * exchangeBitShiftSDiv1(llvm::BasicBlock::iterator iter) const;
	llvm::Instruction * exchangeBitShiftUDiv(llvm::BasicBlock::iterator iter) const;
	llvm::Instruction * exchangeBitShiftMul(llvm::BasicBlock::iterator iter) const;
	llvm::Instruction * exchangeSignedModulo2n(llvm::BasicBlock::iterator iter) const;
	/// Stage 11: Integer abs — (x ^ (x >> (w-1))) - (x >> (w-1)) = abs(x)
	llvm::Instruction * exchangeIntegerAbs(llvm::BasicBlock::iterator iter) const;
};

} // namespace bin2llvmir
} // namespace retdec

#endif
