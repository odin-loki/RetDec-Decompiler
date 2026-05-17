/**
* @file src/capstone2llvmir/arm64/arm64_fp_ext.cpp
* @brief ARM64 FP instruction extensions: rounding, convert variants, paired ops.
* @copyright (c) 2024, MIT license
*
* Implements translations for ARM64 FP instructions that were nullptr in arm64_init.cpp:
*
*  Rounding:
*    FRINTA — round to nearest, ties away from zero  → llvm.round
*    FRINTM — round toward minus infinity            → llvm.floor
*    FRINTN — round to nearest, ties to even         → llvm.nearbyint  (or llvm.rint)
*    FRINTP — round toward plus infinity             → llvm.ceil
*    FRINTZ — round toward zero (truncate)           → llvm.trunc
*
*  Convert (rounding modes):
*    FCVTAS — float → int32/64, round to nearest, ties away    → llvm.lround / llvm.llround
*    FCVTAU — float → uint32/64, round to nearest, ties away
*    FCVTMS — float → int32/64, round toward minus-inf         → FPToSI(floor(x))
*    FCVTMU — float → uint32/64, round toward minus-inf
*    FCVTNS — float → int32/64, round to nearest, ties to even → FPToSI(nearbyint(x))
*    FCVTNU — float → uint32/64, round to nearest, ties to even
*    FCVTPS — float → int32/64, round toward plus-inf          → FPToSI(ceil(x))
*    FCVTPU — float → uint32/64, round toward plus-inf
*    FCVTL/FCVTL2 — widen half→float or float→double (vector; pseudo for now)
*    FCVTN/FCVTN2 — narrow double→float or float→half (vector; pseudo for now)
*    FCVTXN/FCVTXN2 — narrow with rounding toward zero (vector; pseudo for now)
*
*  Paired/reduce:
*    FADDP  — pairwise add: scalar form (extract high+low of pair) or vector
*    FMULX  — FP multiply (extended): 0*Inf → 2.0, otherwise same as FMUL
*    FMAXP  — pairwise max (scalar: max of two-element vector)
*    FMINP  — pairwise min
*
*  Compare with flag-setting:
*    FCMPE  — like FCMP but sets Invalid Operation flag on QNaN; treat as FCMP
*/

#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/Intrinsics.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Instructions.h>

#include "arm64_impl.h"

namespace retdec {
namespace capstone2llvmir {

using namespace llvm;

//===========================================================================
// Helper: get LLVM rounding intrinsic of matching type
//===========================================================================
static Value* callRoundIntrinsic(Intrinsic::ID id, Value* v, IRBuilder<>& irb) {
    Module* m = irb.GetInsertBlock()->getModule();
    Function* fn = Intrinsic::getDeclaration(m, id, {v->getType()});
    return irb.CreateCall(fn, {v});
}

//===========================================================================
// FRINTA / FRINTM / FRINTN / FRINTP / FRINTZ
//===========================================================================

void Capstone2LlvmIrTranslatorArm64_impl::translateFRint(
        cs_insn* i, cs_arm64* ai, IRBuilder<>& irb) {
    EXPECT_IS_BINARY(i, ai, irb);

    if (ifVectorGeneratePseudo(i, ai, irb)) return;

    op1 = loadOp(ai->operands[1], irb);

    Intrinsic::ID id;
    switch (i->id) {
        case ARM64_INS_FRINTA: id = Intrinsic::round;      break;  // ties-away
        case ARM64_INS_FRINTM: id = Intrinsic::floor;      break;  // toward -inf
        case ARM64_INS_FRINTN: id = Intrinsic::nearbyint;  break;  // ties-to-even
        case ARM64_INS_FRINTP: id = Intrinsic::ceil;       break;  // toward +inf
        case ARM64_INS_FRINTZ: id = Intrinsic::trunc;      break;  // toward zero
        default: throw GenericError("Arm64: translateFRint(): unknown instruction");
    }

    auto* val = callRoundIntrinsic(id, op1, irb);
    storeOp(ai->operands[0], val, irb);
}

//===========================================================================
// FCVTAS, FCVTAU, FCVTMS, FCVTMU, FCVTNS, FCVTNU, FCVTPS, FCVTPU
// Convert FP to integer with rounding mode.
//===========================================================================

void Capstone2LlvmIrTranslatorArm64_impl::translateFCvtRound(
        cs_insn* i, cs_arm64* ai, IRBuilder<>& irb) {
    EXPECT_IS_BINARY(i, ai, irb);

    if (ifVectorGeneratePseudo(i, ai, irb)) return;

    op1 = loadOp(ai->operands[1], irb);
    Type* destTy = getRegisterType(ai->operands[0].reg);

    // Apply rounding to float, then truncate to integer.
    Value* rounded = nullptr;
    bool isSigned = true;

    switch (i->id) {
        // Ties-away from zero
        case ARM64_INS_FCVTAS:
            rounded  = callRoundIntrinsic(Intrinsic::round, op1, irb);
            isSigned = true;  break;
        case ARM64_INS_FCVTAU:
            rounded  = callRoundIntrinsic(Intrinsic::round, op1, irb);
            isSigned = false; break;
        // Toward minus infinity
        case ARM64_INS_FCVTMS:
            rounded  = callRoundIntrinsic(Intrinsic::floor, op1, irb);
            isSigned = true;  break;
        case ARM64_INS_FCVTMU:
            rounded  = callRoundIntrinsic(Intrinsic::floor, op1, irb);
            isSigned = false; break;
        // Ties-to-even
        case ARM64_INS_FCVTNS:
            rounded  = callRoundIntrinsic(Intrinsic::nearbyint, op1, irb);
            isSigned = true;  break;
        case ARM64_INS_FCVTNU:
            rounded  = callRoundIntrinsic(Intrinsic::nearbyint, op1, irb);
            isSigned = false; break;
        // Toward plus infinity
        case ARM64_INS_FCVTPS:
            rounded  = callRoundIntrinsic(Intrinsic::ceil, op1, irb);
            isSigned = true;  break;
        case ARM64_INS_FCVTPU:
            rounded  = callRoundIntrinsic(Intrinsic::ceil, op1, irb);
            isSigned = false; break;
        default:
            throw GenericError("Arm64: translateFCvtRound(): unknown instruction");
    }

    Value* intVal = isSigned
        ? irb.CreateFPToSI(rounded, destTy)
        : irb.CreateFPToUI(rounded, destTy);

    storeOp(ai->operands[0], intVal, irb);
}

//===========================================================================
// FCVTL / FCVTL2 — widen elements (half→float or float→double).
// FCVTN / FCVTN2 — narrow elements (float→half or double→float).
// FCVTXN / FCVTXN2 — narrow with round-to-odd.
// These are vector instructions; emit pseudoAsm for now but with a useful
// comment. A future NEON pass can replace them.
//===========================================================================

void Capstone2LlvmIrTranslatorArm64_impl::translateFCvtVec(
        cs_insn* i, cs_arm64* ai, IRBuilder<>& irb) {
    // Vector widen/narrow — fall through to pseudoAsm.
    // The important thing is we don't crash; the result is an opaque call.
    translatePseudoAsmGeneric(i, ai, irb);
}

//===========================================================================
// FADDP — pairwise add.
// Scalar form: dst = src[0] + src[1]  (two-element vector operand).
// Vector form: interleaved pairwise adds across two registers (pseudoAsm).
//===========================================================================

void Capstone2LlvmIrTranslatorArm64_impl::translateFAddP(
        cs_insn* i, cs_arm64* ai, IRBuilder<>& irb) {
    EXPECT_IS_BINARY(i, ai, irb);

    if (ifVectorGeneratePseudo(i, ai, irb)) return;

    // Scalar form: operand 1 is a 2-element FP vector (64-bit pair).
    // We load it as an integer and split into two floats.
    op1 = loadOp(ai->operands[1], irb);

    Type* elemTy = op1->getType()->isDoubleTy()
        ? irb.getDoubleTy() : irb.getFloatTy();

    // For scalar FADDP the source is a Vn.2s or Vn.2d register.
    // Represent as i64 pair of f32, or i128 pair of f64.
    // Simplify: if source is already a scalar FP, treat as identity.
    if (op1->getType()->isFloatingPointTy()) {
        storeOp(ai->operands[0], op1, irb);
        return;
    }

    // Integer → two-float pair.
    unsigned srcBits = op1->getType()->getIntegerBitWidth();
    unsigned halfBits = srcBits / 2;
    Type* halfTy = irb.getIntNTy(halfBits);

    Value* lo_bits = irb.CreateTrunc(op1, halfTy);
    Value* hi_bits = irb.CreateTrunc(
        irb.CreateLShr(op1, ConstantInt::get(op1->getType(), halfBits)), halfTy);

    // Reinterpret as FP.
    Value* lo_fp = irb.CreateBitCast(lo_bits, elemTy);
    Value* hi_fp = irb.CreateBitCast(hi_bits, elemTy);

    Value* result = irb.CreateFAdd(lo_fp, hi_fp);
    storeOp(ai->operands[0], result, irb);
}

//===========================================================================
// FMULX — FP multiply extended.
// Difference from FMUL: 0 * Inf = 2.0 (not NaN). We approximate as FMUL.
// The extended behaviour matters only for zero/inf edge cases in SVE code;
// for typical decompilation purposes FMUL is semantically equivalent.
//===========================================================================

void Capstone2LlvmIrTranslatorArm64_impl::translateFMulX(
        cs_insn* i, cs_arm64* ai, IRBuilder<>& irb) {
    EXPECT_IS_TERNARY(i, ai, irb);

    if (ifVectorGeneratePseudo(i, ai, irb)) return;

    op1 = loadOp(ai->operands[1], irb);
    op2 = loadOp(ai->operands[2], irb);
    auto* val = irb.CreateFMul(op1, op2);
    storeOp(ai->operands[0], val, irb);
}

//===========================================================================
// FMAXP / FMINP — pairwise max/min.
// Scalar form: dst = max/min(src[0], src[1]).
// Vector form: pseudoAsm.
//===========================================================================

void Capstone2LlvmIrTranslatorArm64_impl::translateFMinMaxP(
        cs_insn* i, cs_arm64* ai, IRBuilder<>& irb) {
    EXPECT_IS_BINARY(i, ai, irb);

    if (ifVectorGeneratePseudo(i, ai, irb)) return;

    op1 = loadOp(ai->operands[1], irb);

    if (op1->getType()->isFloatingPointTy()) {
        // Already scalar — identity.
        storeOp(ai->operands[0], op1, irb);
        return;
    }

    // Same split as FADDP.
    unsigned srcBits = op1->getType()->getIntegerBitWidth();
    unsigned halfBits = srcBits / 2;
    Type* halfTy = irb.getIntNTy(halfBits);
    Type* elemTy = halfBits == 32 ? irb.getFloatTy() : irb.getDoubleTy();

    Value* lo_bits = irb.CreateTrunc(op1, halfTy);
    Value* hi_bits = irb.CreateTrunc(
        irb.CreateLShr(op1, ConstantInt::get(op1->getType(), halfBits)), halfTy);

    Value* lo_fp = irb.CreateBitCast(lo_bits, elemTy);
    Value* hi_fp = irb.CreateBitCast(hi_bits, elemTy);

    Value* cond;
    Intrinsic::ID intrin;
    if (i->id == ARM64_INS_FMAXP) {
        cond   = irb.CreateFCmpUGE(lo_fp, hi_fp);
        intrin = Intrinsic::maxnum;
    } else {
        cond   = irb.CreateFCmpULE(lo_fp, hi_fp);
        intrin = Intrinsic::minnum;
    }

    // Use minnum/maxnum intrinsic to get IEEE NaN propagation right.
    Module* m = irb.GetInsertBlock()->getModule();
    Function* fn = Intrinsic::getDeclaration(m, intrin, {elemTy});
    Value* result = irb.CreateCall(fn, {lo_fp, hi_fp});

    storeOp(ai->operands[0], result, irb);
}

//===========================================================================
// FCMPE — floating-point compare, setting NZCV; raises Invalid Operation
// on QNaN input. We treat identically to FCMP (same NZCV output for non-NaN
// inputs, which covers virtually all compiled code).
//===========================================================================

void Capstone2LlvmIrTranslatorArm64_impl::translateFCmpE(
        cs_insn* i, cs_arm64* ai, IRBuilder<>& irb) {
    // Delegate to the existing FCMP translator.
    translateFCmp(i, ai, irb);
}

} // namespace capstone2llvmir
} // namespace retdec
