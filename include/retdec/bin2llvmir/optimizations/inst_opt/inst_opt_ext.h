/**
 * @file include/retdec/bin2llvmir/optimizations/inst_opt/inst_opt_ext.h
 * @copyright (c) 2024, MIT license
 */
#ifndef RETDEC_INST_OPT_EXT_H
#define RETDEC_INST_OPT_EXT_H

#include <llvm/IR/Instruction.h>

namespace retdec {
namespace bin2llvmir {
namespace inst_opt {

bool mulZero(llvm::Instruction* insn);
bool orAllOnes(llvm::Instruction* insn);
bool andZero(llvm::Instruction* insn);
bool subSelf(llvm::Instruction* insn);
bool shiftByZero(llvm::Instruction* insn);
bool selectSame(llvm::Instruction* insn);
bool orAndSelf(llvm::Instruction* insn);

} // namespace inst_opt
} // namespace bin2llvmir
} // namespace retdec

#endif
