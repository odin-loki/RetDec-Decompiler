/**
* @file include/retdec/bin2llvmir/optimizations/global_const_prop/global_const_prop.h
* @brief Read-only global constant propagation.
* @copyright (c) 2024, MIT license
*/

#ifndef RETDEC_BIN2LLVMIR_OPTIMIZATIONS_GLOBAL_CONST_PROP_H
#define RETDEC_BIN2LLVMIR_OPTIMIZATIONS_GLOBAL_CONST_PROP_H

#include <llvm/IR/Module.h>
#include <llvm/Pass.h>

#include "retdec/bin2llvmir/providers/config.h"
#include "retdec/bin2llvmir/providers/fileimage.h"

namespace retdec {
namespace bin2llvmir {

class GlobalConstProp : public llvm::ModulePass {
public:
    static char ID;
    GlobalConstProp();
    virtual bool runOnModule(llvm::Module& m) override;
    bool runOnModuleCustom(llvm::Module& m, Config* config, FileImage* image);

private:
    bool run();

    llvm::Module* _module = nullptr;
    Config*       _config = nullptr;
    FileImage*    _image  = nullptr;
};

} // namespace bin2llvmir
} // namespace retdec

#endif
