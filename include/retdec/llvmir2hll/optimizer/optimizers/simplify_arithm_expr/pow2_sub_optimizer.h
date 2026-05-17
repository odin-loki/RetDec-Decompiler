/**
* @file include/retdec/llvmir2hll/optimizer/optimizers/simplify_arithm_expr/pow2_sub_optimizer.h
* @copyright (c) 2024, MIT license
*/
#ifndef RETDEC_POW2_SUB_OPTIMIZER_H
#define RETDEC_POW2_SUB_OPTIMIZER_H

#include <string>
#include "retdec/llvmir2hll/ir/bit_xor_op_expr.h"
#include "retdec/llvmir2hll/ir/div_op_expr.h"
#include "retdec/llvmir2hll/ir/mod_op_expr.h"
#include "retdec/llvmir2hll/ir/mul_op_expr.h"
#include "retdec/llvmir2hll/ir/neg_op_expr.h"
#include "retdec/llvmir2hll/ir/not_op_expr.h"
#include "retdec/llvmir2hll/optimizer/optimizers/simplify_arithm_expr/sub_optimizer.h"
#include "retdec/llvmir2hll/support/types.h"

namespace retdec {
namespace llvmir2hll {

class Pow2SubOptimizer : public SubOptimizer {
public:
    explicit Pow2SubOptimizer(ShPtr<ArithmExprEvaluator> e);
    static ShPtr<SubOptimizer> create(ShPtr<ArithmExprEvaluator> e);
    std::string getId() const override;

    void visit(ShPtr<MulOpExpr> expr) override;
    void visit(ShPtr<DivOpExpr> expr) override;
    void visit(ShPtr<ModOpExpr> expr) override;
    void visit(ShPtr<NegOpExpr> expr) override;
    void visit(ShPtr<BitXorOpExpr> expr) override;
    void visit(ShPtr<NotOpExpr> expr) override;
};

} // namespace llvmir2hll
} // namespace retdec

#endif
