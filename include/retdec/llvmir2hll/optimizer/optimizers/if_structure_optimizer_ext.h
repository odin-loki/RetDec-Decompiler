/**
* @file include/retdec/llvmir2hll/optimizer/optimizers/if_structure_optimizer_ext.h
* @copyright (c) 2024, MIT license
*/
#ifndef RETDEC_IF_STRUCTURE_OPTIMIZER_EXT_H
#define RETDEC_IF_STRUCTURE_OPTIMIZER_EXT_H

#include "retdec/llvmir2hll/ir/if_stmt.h"
#include "retdec/llvmir2hll/support/types.h"

namespace retdec {
namespace llvmir2hll {

bool tryOptimization6(ShPtr<IfStmt> stmt);
bool tryOptimization7(ShPtr<IfStmt> stmt);

} // namespace llvmir2hll
} // namespace retdec

#endif
