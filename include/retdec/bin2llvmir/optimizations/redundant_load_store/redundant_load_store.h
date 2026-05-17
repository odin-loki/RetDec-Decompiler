/**
 * @file include/retdec/bin2llvmir/optimizations/redundant_load_store/redundant_load_store.h
 * @copyright (c) 2024, MIT license
 */
#ifndef RETDEC_REDUNDANT_LOAD_STORE_H
#define RETDEC_REDUNDANT_LOAD_STORE_H

#include <llvm/IR/BasicBlock.h>
#include <llvm/IR/Module.h>
#include <llvm/Pass.h>

namespace retdec {
namespace bin2llvmir {

class RedundantLoadStoreElim : public llvm::ModulePass {
public:
    static char ID;
    RedundantLoadStoreElim();
    bool runOnModule(llvm::Module& M) override;
private:
    bool runOnBlock(llvm::BasicBlock& BB);
};

} // namespace bin2llvmir
} // namespace retdec

#endif
