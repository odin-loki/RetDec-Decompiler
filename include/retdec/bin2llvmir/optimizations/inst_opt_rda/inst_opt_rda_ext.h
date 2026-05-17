/**
* @file include/retdec/bin2llvmir/optimizations/inst_opt_rda/inst_opt_rda_ext.h
* @brief Additional RDA-based instruction optimizations.
* @copyright (c) 2024, MIT license
*/

#ifndef RETDEC_BIN2LLVMIR_INST_OPT_RDA_EXT_H
#define RETDEC_BIN2LLVMIR_INST_OPT_RDA_EXT_H

#include <unordered_set>
#include <llvm/IR/Instructions.h>

#include "retdec/bin2llvmir/analyses/reaching_definitions.h"
#include "retdec/bin2llvmir/providers/abi/abi.h"

namespace retdec {
namespace bin2llvmir {
namespace inst_opt_rda {

bool constantFoldingThroughLoads(
        llvm::Instruction* insn,
        ReachingDefinitionsAnalysis& RDA,
        Abi* abi,
        std::unordered_set<llvm::Value*>* toRemove);

bool doubleLoadElimination(
        llvm::Instruction* insn,
        ReachingDefinitionsAnalysis& RDA,
        Abi* abi,
        std::unordered_set<llvm::Value*>* toRemove);

bool crossBbStorePropagation(
        llvm::Instruction* insn,
        ReachingDefinitionsAnalysis& RDA,
        Abi* abi,
        std::unordered_set<llvm::Value*>* toRemove);

} // namespace inst_opt_rda
} // namespace bin2llvmir
} // namespace retdec

#endif
