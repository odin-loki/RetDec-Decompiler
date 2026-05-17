/**
* @file include/retdec/bin2llvmir/optimizations/tail_calls/tail_calls.h
* @brief Tail call detection and annotation.
* @copyright (c) 2024, MIT license
*/

#ifndef RETDEC_BIN2LLVMIR_OPTIMIZATIONS_TAIL_CALLS_H
#define RETDEC_BIN2LLVMIR_OPTIMIZATIONS_TAIL_CALLS_H

#include <llvm/IR/Module.h>
#include <llvm/Pass.h>

#include "retdec/bin2llvmir/providers/abi/abi.h"
#include "retdec/bin2llvmir/providers/config.h"

namespace retdec {
namespace bin2llvmir {

class TailCallOptimizer : public llvm::ModulePass {
public:
    static char ID;
    TailCallOptimizer();
    virtual bool runOnModule(llvm::Module& m) override;
    bool runOnModuleCustom(llvm::Module& m, Abi* abi, Config* config);

private:
    bool run();
    bool processFunction(llvm::Function& fn);

    llvm::Module* _module = nullptr;
    Abi*          _abi    = nullptr;
    Config*       _config = nullptr;
};

} // namespace bin2llvmir
} // namespace retdec

#endif
