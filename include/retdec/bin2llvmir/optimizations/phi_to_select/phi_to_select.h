/**
 * @file include/retdec/bin2llvmir/optimizations/phi_to_select/phi_to_select.h
 * @copyright (c) 2024, MIT license
 */
#ifndef RETDEC_PHI_TO_SELECT_H
#define RETDEC_PHI_TO_SELECT_H

#include <llvm/IR/Instructions.h>
#include <llvm/IR/Module.h>
#include <llvm/Pass.h>

namespace retdec {
namespace bin2llvmir {

class PhiToSelect : public llvm::ModulePass {
public:
    static char ID;
    PhiToSelect();
    bool runOnModule(llvm::Module& M) override;
private:
    bool runOnFunction(llvm::Function& F);
    bool tryConvert(llvm::PHINode* phi);
};

} // namespace bin2llvmir
} // namespace retdec

#endif
