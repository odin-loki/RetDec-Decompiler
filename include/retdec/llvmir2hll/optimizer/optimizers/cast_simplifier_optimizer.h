/**
 * @file include/retdec/llvmir2hll/optimizer/optimizers/cast_simplifier_optimizer.h
 * @copyright (c) 2024, MIT license
 */
#ifndef RETDEC_CAST_SIMPLIFIER_OPTIMIZER_H
#define RETDEC_CAST_SIMPLIFIER_OPTIMIZER_H

#include "retdec/llvmir2hll/ir/bit_cast_expr.h"
#include "retdec/llvmir2hll/ir/ext_cast_expr.h"
#include "retdec/llvmir2hll/ir/fp_to_int_cast_expr.h"
#include "retdec/llvmir2hll/ir/int_to_fp_cast_expr.h"
#include "retdec/llvmir2hll/ir/int_to_ptr_cast_expr.h"
#include "retdec/llvmir2hll/ir/ptr_to_int_cast_expr.h"
#include "retdec/llvmir2hll/ir/trunc_cast_expr.h"
#include "retdec/llvmir2hll/optimizer/func_optimizer.h"
#include "retdec/llvmir2hll/support/smart_ptr.h"

namespace retdec {
namespace llvmir2hll {

class CastSimplifierOptimizer : public FuncOptimizer {
public:
    explicit CastSimplifierOptimizer(ShPtr<Module> module);

    std::string getId() const override { return "CastSimplifier"; }

    void visit(ShPtr<BitCastExpr> expr) override;
    void visit(ShPtr<ExtCastExpr> expr) override;
    void visit(ShPtr<TruncCastExpr> expr) override;
    void visit(ShPtr<FPToIntCastExpr> expr) override;
    void visit(ShPtr<IntToFPCastExpr> expr) override;
    void visit(ShPtr<IntToPtrCastExpr> expr) override;

private:
    void trySimplify(ShPtr<CastExpr> castExpr);
    void trySimplifyPtrRoundTrip(ShPtr<IntToPtrCastExpr> outerCast);
};

} // namespace llvmir2hll
} // namespace retdec

#endif
