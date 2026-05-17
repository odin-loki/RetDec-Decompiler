/**
* @file include/retdec/bin2llvmir/optimizations/idioms/idioms_ext.h
* @brief Extended compiler idiom recognition.
* @copyright (c) 2024, MIT license
*/

#ifndef RETDEC_BIN2LLVMIR_OPTIMIZATIONS_IDIOMS_EXT_H
#define RETDEC_BIN2LLVMIR_OPTIMIZATIONS_IDIOMS_EXT_H

#include <llvm/IR/BasicBlock.h>
#include <llvm/IR/Function.h>

namespace retdec {
namespace bin2llvmir {

/// Run extended idiom recognition on a single basic block.
/// Returns true if any instruction was replaced.
bool idiomsExt(llvm::BasicBlock& bb);

/// Run extended idiom recognition across an entire function.
bool idiomsExtFunction(llvm::Function& fn);

} // namespace bin2llvmir
} // namespace retdec

#endif
