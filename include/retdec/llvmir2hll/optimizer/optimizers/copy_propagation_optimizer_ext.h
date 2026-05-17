/**
* @file include/retdec/llvmir2hll/optimizer/optimizers/copy_propagation_optimizer_ext.h
* @copyright (c) 2024, MIT license
*/
#ifndef RETDEC_COPY_PROPAGATION_OPT_EXT_H
#define RETDEC_COPY_PROPAGATION_OPT_EXT_H

#include "retdec/llvmir2hll/ir/module.h"
#include "retdec/llvmir2hll/ir/statement.h"
#include "retdec/llvmir2hll/ir/variable.h"
#include "retdec/llvmir2hll/analysis/value_analysis.h"
#include "retdec/llvmir2hll/support/types.h"

namespace retdec {
namespace llvmir2hll {

bool canPropagateThroughStmts(ShPtr<Statement> stmt,
                               ShPtr<Variable> lhsVar,
                               ShPtr<Statement> use,
                               ShPtr<Module> module,
                               ShPtr<ValueAnalysis> va);

} // namespace llvmir2hll
} // namespace retdec

#endif
