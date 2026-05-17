/**
 * @file include/retdec/bin2llvmir/optimizations/entry_alloca/entry_alloca.h
 * @brief Ensure all constant-size allocas live in function entry blocks.
 * @copyright (c) 2017 Odin Loch Trading as Imortek
 */

#ifndef RETDEC_BIN2LLVMIR_OPTIMIZATIONS_ENTRY_ALLOCA_H
#define RETDEC_BIN2LLVMIR_OPTIMIZATIONS_ENTRY_ALLOCA_H

#include <llvm/IR/Module.h>
#include <llvm/Pass.h>

namespace retdec {
namespace bin2llvmir {

/**
 * LLVM module pass that moves every constant-size alloca instruction to the
 * function's entry block.  LLVM's simplifycfg can occasionally sink allocas
 * into non-entry blocks, which violates the dominance invariant required by
 * mem2reg and the module verifier.  Running this pass just before 'verify'
 * in the pipeline ensures the IR is well-formed.
 */
class EntryAlloca : public llvm::ModulePass
{
public:
	static char ID;
	EntryAlloca();

	bool runOnModule(llvm::Module& M) override;
};

} // namespace bin2llvmir
} // namespace retdec

#endif // RETDEC_BIN2LLVMIR_OPTIMIZATIONS_ENTRY_ALLOCA_H
