/**
* @file include/retdec/llvmir2hll/var_renamer/var_renamers/debug_var_renamer.h
* @brief Variable renamer that aggressively applies DWARF debug names.
* @copyright (c) 2024, MIT license
*/

#ifndef RETDEC_LLVMIR2HLL_VAR_RENAMER_DEBUG_VAR_RENAMER_H
#define RETDEC_LLVMIR2HLL_VAR_RENAMER_DEBUG_VAR_RENAMER_H

#include "retdec/llvmir2hll/var_renamer/var_renamer.h"
#include "retdec/llvmir2hll/support/smart_ptr.h"

namespace retdec {
namespace llvmir2hll {

class VarNameGen;

class DebugVarRenamer : public VarRenamer {
public:
    static ShPtr<VarRenamer> create(ShPtr<VarNameGen> varNameGen,
                                    bool useDebugNames);
    virtual std::string getId() const override;

protected:
    DebugVarRenamer(ShPtr<VarNameGen> varNameGen, bool useDebugNames);
    virtual void doVarsRenaming() override;
};

} // namespace llvmir2hll
} // namespace retdec

#endif
