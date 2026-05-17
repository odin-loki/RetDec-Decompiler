/**
* @file include/retdec/bin2llvmir/optimizations/cond_branch_opt/cond_branch_opt_ext.h
* @copyright (c) 2024, MIT license
*/
#ifndef RETDEC_COND_BRANCH_OPT_EXT_H
#define RETDEC_COND_BRANCH_OPT_EXT_H

#include <llvm/IR/Instructions.h>

namespace retdec {
namespace bin2llvmir {

/// Run extended conditional branch patterns on @a inst.
/// Returns true if @a inst was replaced and erased.
bool condBranchOptExt(llvm::Instruction& inst);

} // namespace bin2llvmir
} // namespace retdec

#endif
