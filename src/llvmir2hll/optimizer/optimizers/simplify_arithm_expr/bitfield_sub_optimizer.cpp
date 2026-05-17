/**
* @file src/llvmir2hll/optimizer/optimizers/simplify_arithm_expr/bitfield_sub_optimizer.cpp
* @brief Simplify bitfield access arithmetic patterns.
* @copyright (c) 2024, MIT license
*
* Compilers frequently emit bitfield accesses as:
*
*   (x >> shift) & mask       -- extract
*   (x & mask) >> shift       -- extract (alternate order)
*   (x & ~mask) | (val << shift) -- insert
*
* And common constant-folding leftovers:
*
*   x & 0xFFFFFFFF            -- identity mask (no-op on 32-bit)
*   x & 0                     -- always 0
*   x | 0                     -- identity
*   x ^ 0                     -- identity
*   x >> 0  /  x << 0         -- identity
*   (x << N) >> N             -- zero-extension (→ cast to smaller type)
*   x & (2^N - 1) where N = type width -- identity
*
* This sub-optimizer handles all of the above at the HLL-IR expression level.
*/

#include "retdec/llvmir2hll/evaluator/arithm_expr_evaluator.h"
#include "retdec/llvmir2hll/ir/bit_and_op_expr.h"
#include "retdec/llvmir2hll/ir/bit_or_op_expr.h"
#include "retdec/llvmir2hll/ir/bit_shl_op_expr.h"
#include "retdec/llvmir2hll/ir/bit_shr_op_expr.h"
#include "retdec/llvmir2hll/ir/bit_xor_op_expr.h"
#include "retdec/llvmir2hll/ir/const_int.h"
#include "retdec/llvmir2hll/ir/int_type.h"
#include "retdec/llvmir2hll/optimizer/optimizers/simplify_arithm_expr/bitfield_sub_optimizer.h"
#include "retdec/llvmir2hll/support/debug.h"

namespace retdec {
namespace llvmir2hll {

REGISTER_AT_FACTORY("Bitfield", BITFIELD_SUB_OPTIMIZER_ID,
    SubOptimizerFactory, BitfieldSubOptimizer::create);

BitfieldSubOptimizer::BitfieldSubOptimizer(
        ShPtr<ArithmExprEvaluator> arithmExprEvaluator)
    : SubOptimizer(arithmExprEvaluator) {}

ShPtr<SubOptimizer> BitfieldSubOptimizer::create(
        ShPtr<ArithmExprEvaluator> arithmExprEvaluator) {
    return ShPtr<SubOptimizer>(new BitfieldSubOptimizer(arithmExprEvaluator));
}

std::string BitfieldSubOptimizer::getId() const {
    return BITFIELD_SUB_OPTIMIZER_ID;
}

//===========================================================================
// Helpers
//===========================================================================

/// Return the bit-width of @a expr's integer type, or 0 if not integer.
static unsigned exprBitWidth(ShPtr<Expression> expr) {
    if (!expr) return 0;
    if (auto it = cast<IntType>(expr->getType()))
        return it->getSize();
    return 0;
}

/// Return the value of @a ci as a uint64_t, or 0 on failure.
static uint64_t constVal(ShPtr<ConstInt> ci) {
    if (!ci) return 0;
    return ci->getValue().getZExtValue();
}

/// True if @a mask is an all-ones mask for a type of @a bits width.
static bool isAllOnes(uint64_t mask, unsigned bits) {
    if (bits == 0 || bits > 64) return false;
    if (bits == 64) return mask == UINT64_MAX;
    return mask == ((uint64_t(1) << bits) - 1);
}

//===========================================================================
// BitAnd optimizations
//===========================================================================

void BitfieldSubOptimizer::visit(ShPtr<BitAndOpExpr> expr) {
    OrderedAllVisitor::visit(expr);

    auto lhs = expr->getFirstOperand();
    auto rhs = expr->getSecondOperand();

    ShPtr<ConstInt> rhsCI(cast<ConstInt>(rhs));
    ShPtr<ConstInt> lhsCI(cast<ConstInt>(lhs));
    ShPtr<ConstInt> maskCI = rhsCI ? rhsCI : lhsCI;
    ShPtr<Expression> other = rhsCI ? lhs : (lhsCI ? rhs : ShPtr<Expression>());

    if (!maskCI || !other) return;

    uint64_t mask  = constVal(maskCI);
    unsigned width = exprBitWidth(other);

    // x & 0 → 0
    if (mask == 0) {
        optimizeExpr(expr, ConstInt::create(0, width > 0 ? width : 32, false));
        return;
    }

    // x & all_ones → x   (e.g. x & 0xFFFFFFFF when x is 32-bit)
    if (width > 0 && isAllOnes(mask, width)) {
        optimizeExpr(expr, other);
        return;
    }

    // (x >> shift) & mask where mask == (1 << (width - shift)) - 1
    // This is a zero-extension pattern — leave it; it's clearer as-is.
    // Future: could emit a cast to a narrower type.
}

//===========================================================================
// BitOr optimizations
//===========================================================================

void BitfieldSubOptimizer::visit(ShPtr<BitOrOpExpr> expr) {
    OrderedAllVisitor::visit(expr);

    auto lhs = expr->getFirstOperand();
    auto rhs = expr->getSecondOperand();

    // x | 0 → x
    ShPtr<ConstInt> rhsCI(cast<ConstInt>(rhs));
    ShPtr<ConstInt> lhsCI(cast<ConstInt>(lhs));

    if (rhsCI && constVal(rhsCI) == 0) {
        optimizeExpr(expr, lhs);
        return;
    }
    if (lhsCI && constVal(lhsCI) == 0) {
        optimizeExpr(expr, rhs);
        return;
    }

    // x | all_ones → all_ones
    unsigned width = exprBitWidth(lhs);
    if (rhsCI && width > 0 && isAllOnes(constVal(rhsCI), width)) {
        optimizeExpr(expr, rhsCI);
        return;
    }
}

//===========================================================================
// BitXor optimizations
//===========================================================================

void BitfieldSubOptimizer::visit(ShPtr<BitXorOpExpr> expr) {
    OrderedAllVisitor::visit(expr);

    auto lhs = expr->getFirstOperand();
    auto rhs = expr->getSecondOperand();

    // x ^ 0 → x
    ShPtr<ConstInt> rhsCI(cast<ConstInt>(rhs));
    ShPtr<ConstInt> lhsCI(cast<ConstInt>(lhs));

    if (rhsCI && constVal(rhsCI) == 0) {
        optimizeExpr(expr, lhs);
        return;
    }
    if (lhsCI && constVal(lhsCI) == 0) {
        optimizeExpr(expr, rhs);
        return;
    }

    // x ^ x → 0  (same operand)
    if (lhs->isEqualTo(rhs)) {
        unsigned width = exprBitWidth(lhs);
        optimizeExpr(expr, ConstInt::create(0, width > 0 ? width : 32, false));
        return;
    }
}

//===========================================================================
// Shift optimizations
//===========================================================================

void BitfieldSubOptimizer::visit(ShPtr<BitShlOpExpr> expr) {
    OrderedAllVisitor::visit(expr);

    ShPtr<ConstInt> shiftCI(cast<ConstInt>(expr->getSecondOperand()));
    if (!shiftCI) return;

    // x << 0 → x
    if (constVal(shiftCI) == 0) {
        optimizeExpr(expr, expr->getFirstOperand());
        return;
    }

    // x << N where N >= bit_width → 0
    unsigned width = exprBitWidth(expr->getFirstOperand());
    if (width > 0 && constVal(shiftCI) >= width) {
        optimizeExpr(expr, ConstInt::create(0, width, false));
        return;
    }
}

void BitfieldSubOptimizer::visit(ShPtr<BitShrOpExpr> expr) {
    OrderedAllVisitor::visit(expr);

    ShPtr<ConstInt> shiftCI(cast<ConstInt>(expr->getSecondOperand()));
    if (!shiftCI) return;

    // x >> 0 → x
    if (constVal(shiftCI) == 0) {
        optimizeExpr(expr, expr->getFirstOperand());
        return;
    }

    // x >> N where N >= bit_width → 0 (logical) or sign_bit (arithmetic)
    // For logical shift only:
    unsigned width = exprBitWidth(expr->getFirstOperand());
    if (width > 0 && constVal(shiftCI) >= width && !expr->isArithmetical()) {
        optimizeExpr(expr, ConstInt::create(0, width, false));
        return;
    }

    // (x << N) >> N → x & mask  (zero-extension; simplify to just x if same
    // type, since the type system already tracks width)
    if (auto shl = cast<BitShlOpExpr>(expr->getFirstOperand())) {
        ShPtr<ConstInt> shlShiftCI(cast<ConstInt>(shl->getSecondOperand()));
        if (shlShiftCI && constVal(shlShiftCI) == constVal(shiftCI)) {
            // Only if logical shr (not arithmetic).
            if (!expr->isArithmetical()) {
                // The inner value with a mask applied.
                uint64_t n    = constVal(shiftCI);
                uint64_t mask = (n < 64) ? ((uint64_t(1) << (width - n)) - 1) : 0;
                auto maskExpr = ConstInt::create(
                    static_cast<int64_t>(mask), width, false);
                auto andExpr  = BitAndOpExpr::create(shl->getFirstOperand(),
                                                      maskExpr);
                optimizeExpr(expr, andExpr);
                return;
            }
        }
    }
}

} // namespace llvmir2hll
} // namespace retdec
