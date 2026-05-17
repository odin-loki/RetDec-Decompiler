/**
* @file include/retdec/llvmir2hll/analysis/alias_analysis/alias_analyses/simple_alias_analysis_ext.h
* @copyright (c) 2024, MIT license
*/
#ifndef RETDEC_SIMPLE_ALIAS_ANALYSIS_EXT_H
#define RETDEC_SIMPLE_ALIAS_ANALYSIS_EXT_H

#include <map>
#include "retdec/llvmir2hll/ir/function.h"
#include "retdec/llvmir2hll/ir/module.h"
#include "retdec/llvmir2hll/ir/variable.h"
#include "retdec/llvmir2hll/support/types.h"

namespace retdec {
namespace llvmir2hll {

void buildSeparatedAddressedSets(
        ShPtr<Module> module,
        VarSet& outGlobal,
        std::map<ShPtr<Function>, VarSet>& outFuncLocal,
        VarSet& outEscaped);

VarSet filterByPointerElementType(ShPtr<Variable> ptr,
                                   const VarSet& candidates);

ShPtr<Variable> singleAssignPointsTo(ShPtr<Variable> ptrVar,
                                      ShPtr<Function> fn);

} // namespace llvmir2hll
} // namespace retdec

#endif
