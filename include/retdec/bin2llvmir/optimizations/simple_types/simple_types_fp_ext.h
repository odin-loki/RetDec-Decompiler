/**
* @file include/retdec/bin2llvmir/optimizations/simple_types/simple_types_fp_ext.h
* @brief Helper routines for simple type propagation extensions.
*/

#ifndef RETDEC_BIN2LLVMIR_OPTIMIZATIONS_SIMPLE_TYPES_SIMPLE_TYPES_FP_EXT_H
#define RETDEC_BIN2LLVMIR_OPTIMIZATIONS_SIMPLE_TYPES_SIMPLE_TYPES_FP_EXT_H

#include <queue>

#include <llvm/IR/Instructions.h>

namespace retdec {
namespace bin2llvmir {

bool simpleTypesFpExt(llvm::Instruction* user, std::queue<llvm::Value*>& toProcess);

} // namespace bin2llvmir
} // namespace retdec

#endif
