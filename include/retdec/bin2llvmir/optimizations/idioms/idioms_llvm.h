/**
* @file include/retdec/bin2llvmir/optimizations/idioms/idioms_llvm.h
* @brief clang/LLVM instruction idioms
* @copyright (c) 2017 Odin Loch Trading as Imortek
*/

#ifndef RETDEC_BIN2LLVMIR_OPTIMIZATIONS_IDIOMS_IDIOMS_LLVM_H
#define RETDEC_BIN2LLVMIR_OPTIMIZATIONS_IDIOMS_IDIOMS_LLVM_H

#include "retdec/bin2llvmir/optimizations/idioms/idioms_abstract.h"

namespace retdec {
namespace bin2llvmir {

/**
 * @brief clang/LLVM instruction idioms
 */
class IdiomsLLVM: virtual public IdiomsAbstract {
	friend class IdiomsAnalysis;

protected:
	llvm::Instruction * exchangeIsGreaterThanMinusOne(llvm::BasicBlock::iterator iter) const;
	llvm::Instruction * exchangeCompareEq(llvm::BasicBlock::iterator iter) const;
	llvm::Instruction * exchangeCompareNeq(llvm::BasicBlock::iterator iter) const;
	llvm::Instruction * exchangeCompareSlt(llvm::BasicBlock::iterator iter) const;
	llvm::Instruction * exchangeCompareSle(llvm::BasicBlock::iterator iter) const;
};

} // namespace bin2llvmir
} // namespace retdec

#endif
