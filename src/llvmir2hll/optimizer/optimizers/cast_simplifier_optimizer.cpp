/**
 * @file src/llvmir2hll/optimizer/optimizers/cast_simplifier_optimizer.cpp
 * @brief Multi-pattern cast simplification optimizer.
 * @copyright (c) 2024, MIT license
 *
 * Implements patterns that RemoveUselessCastsOptimizer (case 1 only) and
 * CCastOptimizer missed:
 *
 *  Case 2 — Idempotent cast chain:
 *    cast<T>(cast<T>(x))  →  cast<T>(x)
 *    Rationale: two consecutive casts to the same destination type can be
 *    collapsed to a single cast. E.g. (int32)(int32)(x) → (int32)(x).
 *
 *  Case 3 — Identity cast (no-op):
 *    cast<T>(x) where type(x) == T  →  x
 *    Rationale: the cast does nothing. Extremely common from LLVM IR where
 *    bitcasts of already-correct types are emitted.
 *
 *  Case 4 — Pointer round-trip:
 *    inttoptr(ptrtoint(p))  →  p   (when source and destination pointer types match)
 *    Rationale: pointer→integer→pointer round-trip is a no-op in C.
 *
 *  Case 5 — Widening followed by narrowing (zext+trunc → mask):
 *    Already in inst_opt (truncZext at LLVM IR level). This case handles the
 *    HLL-level residual: ExtCast<M>(TruncCast<N>(x)) where M==original width
 *    → x & ((1<<N)-1).
 *    Only applied when the ExtCast is zero-extending (unsigned).
 *
 *  Case 6 — Chained sign/zero cast simplification:
 *    cast<int32>(cast<int64>(x))  where sizeof(x)==32  →  x
 *    (downcasting then upcasting to original size cancels)
 *
 * Runs as a FuncOptimizer over the whole module.
 */

#include "retdec/llvmir2hll/ir/bit_and_op_expr.h"
#include "retdec/llvmir2hll/ir/fp_to_int_cast_expr.h"
#include "retdec/llvmir2hll/ir/int_to_fp_cast_expr.h"
#include "retdec/llvmir2hll/ir/bit_cast_expr.h"
#include "retdec/llvmir2hll/ir/const_int.h"
#include "retdec/llvmir2hll/ir/ext_cast_expr.h"
#include "retdec/llvmir2hll/ir/int_to_ptr_cast_expr.h"
#include "retdec/llvmir2hll/ir/int_type.h"
#include "retdec/llvmir2hll/ir/pointer_type.h"
#include "retdec/llvmir2hll/ir/ptr_to_int_cast_expr.h"
#include "retdec/llvmir2hll/ir/trunc_cast_expr.h"
#include "retdec/llvmir2hll/optimizer/optimizers/cast_simplifier_optimizer.h"
#include "retdec/llvmir2hll/support/debug.h"

namespace retdec {
namespace llvmir2hll {

CastSimplifierOptimizer::CastSimplifierOptimizer(ShPtr<Module> module)
    : FuncOptimizer(module) {
    PRECONDITION_NON_NULL(module);
}

void CastSimplifierOptimizer::visit(ShPtr<BitCastExpr> expr) {
    FuncOptimizer::visit(expr);
    trySimplify(expr);
}
void CastSimplifierOptimizer::visit(ShPtr<ExtCastExpr> expr) {
    FuncOptimizer::visit(expr);
    trySimplify(expr);
}
void CastSimplifierOptimizer::visit(ShPtr<TruncCastExpr> expr) {
    FuncOptimizer::visit(expr);
    trySimplify(expr);
}
void CastSimplifierOptimizer::visit(ShPtr<FPToIntCastExpr> expr) {
    FuncOptimizer::visit(expr);
    trySimplify(expr);
}
void CastSimplifierOptimizer::visit(ShPtr<IntToFPCastExpr> expr) {
    FuncOptimizer::visit(expr);
    trySimplify(expr);
}
void CastSimplifierOptimizer::visit(ShPtr<IntToPtrCastExpr> expr) {
    FuncOptimizer::visit(expr);
    trySimplifyPtrRoundTrip(expr);
}

/**
 * Attempt to simplify @a castExpr using cases 2, 3, 5, 6.
 */
void CastSimplifierOptimizer::trySimplify(ShPtr<CastExpr> castExpr) {
    auto dstType = castExpr->getType();
    auto operand = castExpr->getOperand();

    // ── Case 3: identity cast  cast<T>(x) where type(x)==T  →  x ──────────
    if (operand->getType() == dstType) {
        Expression::replaceExpression(castExpr, operand);
        return;
    }

    // ── Case 2: idempotent  cast<T>(cast<T>(x))  →  cast<T>(x) ────────────
    if (auto innerCast = cast<CastExpr>(operand)) {
        if (innerCast->getType() == dstType) {
            // Outer and inner cast to same type — outer is redundant.
            Expression::replaceExpression(castExpr, innerCast);
            return;
        }

        // ── Case 5: zext(trunc(x,N)) where outer type == original type ────
        if (auto extCast = cast<ExtCastExpr>(castExpr)) {
            if (extCast->getVariant() != ExtCastExpr::Variant::ZExt) return;   // only unsigned widening
            if (auto truncInner = cast<TruncCastExpr>(innerCast)) {
                auto srcTy  = cast<IntType>(truncInner->getOperand()->getType());
                auto truncTy = cast<IntType>(truncInner->getType());
                auto dstIntTy = cast<IntType>(dstType);
                if (srcTy && truncTy && dstIntTy &&
                    dstIntTy->getSize() == srcTy->getSize()) {
                    // zext<M>(trunc<N>(x)) where M==original size → x & mask
                    unsigned n = truncTy->getSize();
                    uint64_t mask = (n >= 64) ? ~0ULL : (1ULL << n) - 1;
                    llvm::APInt apMask(srcTy->getSize(), mask);
                    auto maskConst = ConstInt::create(apMask, false);
                    auto andExpr = BitAndOpExpr::create(
                        truncInner->getOperand(), maskConst);
                    Expression::replaceExpression(castExpr, andExpr);
                    return;
                }
            }
        }

        // ── Case 6: int32(int64(x)) where sizeof(x)==32  →  x ───────────
        if (auto dstInt = cast<IntType>(dstType)) {
            if (auto innerInt = cast<IntType>(innerCast->getType())) {
                auto innerOp = innerCast->getOperand();
                if (auto srcInt = cast<IntType>(innerOp->getType())) {
                    // If outer cast shrinks back to original size, cancel both.
                    if (dstInt->getSize() == srcInt->getSize()) {
                        Expression::replaceExpression(castExpr, innerOp);
                        return;
                    }
                }

                // ── Case 7: same-width sign reinterpretation collapse ─────
                // (int32_t)(uint32_t)x  →  (int32_t)x
                // The inner cast truncates to N bits; if the outer cast also
                // targets N bits (possibly with a different sign), the inner
                // cast is redundant because the bit pattern is already N bits
                // wide. The outer cast then reinterprets directly from the
                // source, which is semantically identical.
                if (dstInt->getSize() == innerInt->getSize()) {
                    // Both casts target the same bit width — collapse the inner
                    // one by applying the outer cast directly to the operand.
                    auto newCast = TruncCastExpr::create(innerCast->getOperand(), dstInt);
                    Expression::replaceExpression(castExpr, newCast);
                    return;
                }
            }
        }
    }
}

/**
 * Case 4: inttoptr(ptrtoint(p)) → p
 */
void CastSimplifierOptimizer::trySimplifyPtrRoundTrip(
        ShPtr<IntToPtrCastExpr> outerCast) {
    auto innerCast = cast<PtrToIntCastExpr>(outerCast->getOperand());
    if (!innerCast) return;

    auto origPtr  = innerCast->getOperand();
    auto origType = origPtr->getType();
    auto dstType  = outerCast->getType();

    // Only collapse when pointer types match.
    if (origType == dstType) {
        Expression::replaceExpression(outerCast, origPtr);
    }
}

} // namespace llvmir2hll
} // namespace retdec
