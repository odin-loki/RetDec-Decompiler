/**
* @file include/retdec/bin2llvmir/optimizations/jump_table_recovery/jump_table_recovery.h
* @brief Recover switch statements from indirect jump-table branches.
* @copyright (c) 2024, MIT license
*
* Compiled switch statements often produce an indirect branch pattern:
*
*   %idx = sub i32 %val, base_val        ; normalise to 0-based
*   %cmp = icmp ugt i32 %idx, num_cases  ; bounds check
*   br i1 %cmp, label %default, label %dispatch
*
*   dispatch:
*     %ptr = getelementptr [N x i8*], [N x i8*]* @table, i32 0, i32 %idx
*     %addr = load i8*, i8** %ptr
*     indirectbr i8* %addr, [label %case0, label %case1, ...]
*
* This pass detects that pattern in the LLVM IR (post-lifting) and replaces
* the indirectbr + table GEP with an llvm::SwitchInst so that downstream
 * passes (and llvmir2hll) can emit clean switch statements.
 *
 * Extensions (framework Cat. 1.3):
 *  - Validates that the GEP uses a non-constant index (the switch key).
 *  - Strips bitcast/addrspacecast on the table global when needed.
 *  - Peels @c inttoptr / @c bitcast on the indirect branch address before the
 *    @c load (integer jump tables).
 *  - Reads table initializers as @c ConstantArray or @c ConstantDataArray.
 *  - Logs when indirectbr successor count differs from table slot count, or
 *    when the default successor is guessed from the first case; module summary
 *    counts **`ibr_succ_mismatch`** and **`default_case_fallback`** under
 *    **`RETDEC_HEURISTIC_DIAG`**.
 *  - Binary-search (decision-tree) switch lowering is not handled here; that
 *    remains future work (often appears as a chain of compares, not this IR).
 */

#ifndef RETDEC_BIN2LLVMIR_OPTIMIZATIONS_JUMP_TABLE_RECOVERY_H
#define RETDEC_BIN2LLVMIR_OPTIMIZATIONS_JUMP_TABLE_RECOVERY_H

#include <cstdint>
#include <vector>

#include <llvm/IR/Module.h>
#include <llvm/Pass.h>

#include "retdec/bin2llvmir/providers/abi/abi.h"
#include "retdec/bin2llvmir/providers/config.h"
#include "retdec/bin2llvmir/providers/fileimage.h"

namespace retdec {
namespace bin2llvmir {

class JumpTableRecovery : public llvm::ModulePass {
public:
    static char ID;
    JumpTableRecovery();
    virtual bool runOnModule(llvm::Module& m) override;
    bool runOnModuleCustom(llvm::Module& m, Abi* abi, Config* config,
                           FileImage* image);

private:
    bool run();
    bool tryRecoverFunction(llvm::Function& fn);
    bool tryRecoverIndirectBr(llvm::IndirectBrInst* ibr);

    /// Extract (base_value, num_cases, table_global) from the chain
    /// leading into @a ibr. Returns false if pattern not recognised.
    bool analyzeDispatch(llvm::IndirectBrInst*  ibr,
                         llvm::Value*&          switchVal,
                         int64_t&               baseVal,
                         uint64_t&              numCases,
                         llvm::GlobalVariable*& tableGV);

    /// Read the jump table from the binary image and return the ordered
    /// list of target basic blocks.
    bool readTable(llvm::GlobalVariable* tableGV,
                   uint64_t              numCases,
                   llvm::Function&       fn,
                   std::vector<llvm::BasicBlock*>& targets);

private:
    llvm::Module* _module = nullptr;
    Abi*          _abi    = nullptr;
    Config*       _config = nullptr;
    FileImage*    _image  = nullptr;

    /// Counters for one @c run(); emitted under @c RETDEC_HEURISTIC_DIAG.
    unsigned _statIndirectBrCandidates = 0;
    unsigned _statAnalyzeDispatchFail = 0;
    unsigned _statReadTableFail = 0;
    unsigned _statRecovered = 0;
    /// Recovered switch but @c indirectbr successor count ≠ table slot count.
    unsigned _statIbrSuccessorMismatch = 0;
    /// Default case taken as first table target (no bounds-check predecessor).
    unsigned _statDefaultCaseFallback = 0;
};

} // namespace bin2llvmir
} // namespace retdec

#endif
