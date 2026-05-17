/**
* @file include/retdec/llvmir2hll/optimizer/optimizers/simplify_arithm_expr/bitfield_sub_optimizer.h
* @brief Simplify bitfield access arithmetic patterns.
* @copyright (c) 2024, MIT license
*/

#ifndef RETDEC_LLVMIR2HLL_OPTIMIZER_BITFIELD_SUB_OPTIMIZER_H
#define RETDEC_LLVMIR2HLL_OPTIMIZER_BITFIELD_SUB_OPTIMIZER_H

#include "retdec/llvmir2hll/optimizer/optimizers/simplify_arithm_expr/sub_optimizer.h"
#include "retdec/llvmir2hll/support/smart_ptr.h"
#include "retdec/llvmir2hll/support/visitor_adapter.h"

namespace retdec {
namespace llvmir2hll {

class BitfieldSubOptimizer : public SubOptimizer {
public:
    explicit BitfieldSubOptimizer(ShPtr<ArithmExprEvaluator> arithmExprEvaluator);
    static ShPtr<SubOptimizer> create(ShPtr<ArithmExprEvaluator> arithmExprEvaluator);
    virtual std::string getId() const override;

    virtual void visit(ShPtr<BitAndOpExpr> expr) override;
    virtual void visit(ShPtr<BitOrOpExpr>  expr) override;
    virtual void visit(ShPtr<BitXorOpExpr> expr) override;
    virtual void visit(ShPtr<BitShlOpExpr> expr) override;
    virtual void visit(ShPtr<BitShrOpExpr> expr) override;
};

} // namespace llvmir2hll
} // namespace retdec

#endif
