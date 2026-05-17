/**
* @file include/retdec/llvmir2hll/optimizer/optimizers/simplify_arithm_expr/mba_sub_optimizer.h
* @brief Mixed Boolean-Arithmetic (MBA) deobfuscation sub-optimizer.
* @copyright (c) 2024, MIT license
*
* Stage 24: Simplifies common MBA-obfuscated expressions:
*  (a | b) - (a & b)  →  a ^ b
*  (a | b) + (a & b)  →  a + b
*  (a ^ b) + 2*(a & b)  →  a + b
*  (a & b) + (a ^ b)  →  a | b
*/

#ifndef RETDEC_LLVMIR2HLL_OPTIMIZER_OPTIMIZERS_SIMPLIFY_ARITHM_EXPR_MBA_SUB_OPTIMIZER_H
#define RETDEC_LLVMIR2HLL_OPTIMIZER_OPTIMIZERS_SIMPLIFY_ARITHM_EXPR_MBA_SUB_OPTIMIZER_H

#include "retdec/llvmir2hll/ir/add_op_expr.h"
#include "retdec/llvmir2hll/ir/bit_xor_op_expr.h"
#include "retdec/llvmir2hll/ir/sub_op_expr.h"
#include "retdec/llvmir2hll/optimizer/optimizers/simplify_arithm_expr/sub_optimizer.h"
#include "retdec/llvmir2hll/support/types.h"

namespace retdec {
namespace llvmir2hll {

class MbaSubOptimizer : public SubOptimizer {
public:
	explicit MbaSubOptimizer(ShPtr<ArithmExprEvaluator> e);
	static ShPtr<SubOptimizer> create(ShPtr<ArithmExprEvaluator> e);
	std::string getId() const override;

	void visit(ShPtr<SubOpExpr> expr) override;
	void visit(ShPtr<AddOpExpr> expr) override;
	void visit(ShPtr<BitXorOpExpr> expr) override;
};

} // namespace llvmir2hll
} // namespace retdec

#endif
