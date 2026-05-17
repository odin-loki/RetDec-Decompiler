/**
* @file include/retdec/bin2llvmir/optimizations/strength_reduction/strength_reduction.h
* @copyright (c) 2024, MIT license
*/
#ifndef RETDEC_STRENGTH_REDUCTION_H
#define RETDEC_STRENGTH_REDUCTION_H

#include <llvm/IR/Module.h>
#include <llvm/Pass.h>

namespace retdec {
namespace bin2llvmir {

class StrengthReduction : public llvm::ModulePass {
public:
    static char ID;
    StrengthReduction();
    bool runOnModule(llvm::Module& M) override;
private:
    bool runOnFunction(llvm::Function& F);
};

} // namespace bin2llvmir
} // namespace retdec

#endif
