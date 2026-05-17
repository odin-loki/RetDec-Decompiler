/**
* @file include/retdec/llvmir2hll/optimizer/optimizers/while_true_to_for_loop_optimizer_ext.h
* @brief Extensions for WhileTrueToForLoopOptimizer.
* @copyright (c) 2024, MIT license
*/
#ifndef RETDEC_WHILE_TRUE_TO_FOR_LOOP_OPT_EXT_H
#define RETDEC_WHILE_TRUE_TO_FOR_LOOP_OPT_EXT_H

#include "retdec/llvmir2hll/ir/expression.h"
#include "retdec/llvmir2hll/ir/variable.h"
#include "retdec/llvmir2hll/support/types.h"

namespace retdec {
namespace llvmir2hll {

ShPtr<Expression> computeStepExt(ShPtr<Expression> updateRhs,
                                  ShPtr<Variable> indVar);
bool isNonNegativeExt(ShPtr<Expression> expr);
bool isPositiveExt(ShPtr<Expression> expr);

} // namespace llvmir2hll
} // namespace retdec

#endif
