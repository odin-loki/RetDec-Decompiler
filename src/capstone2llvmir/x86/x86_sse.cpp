/**
* @file src/capstone2llvmir/x86/x86_sse.cpp
* @brief SSE/SSE2/SSE3 instruction translation for the x86 capstone2llvmir lifter.
* @copyright (c) 2024, MIT license
*
* Implements translations for the SSE/SSE2 instructions that were previously
* registered as `nullptr` (i.e. falling through to pseudoAsmGeneric) in
* x86_init.cpp.
*
* Covered instructions:
*   Moves:   MOVAPS, MOVAPD, MOVDQA, MOVDQU, MOVD, MOVQ, MOVSS, MOVSD,
*            MOVLHPS, MOVHLPS, VMOVAPS, VMOVAPD
*   Integer: PADDB/W/D/Q, PSUBB/W/D/Q, PAND, PANDN, POR, PXOR,
*            PCMPEQB/W/D, PUNPCKLBW, PUNPCKLDQ
*   Float:   ADDPS, ADDSS, SUBPS, SUBSS(?), MULPS, MULSS, DIVPS, DIVSS,
*            ADDSUBPS, HADDPS, HSUBPS
*   Convert: CVTSI2SS, CVTSS2SI, CVTSI2SD, CVTSD2SI, CVTDQ2PS, CVTPS2DQ,
*            VCVTSI2SS, VCVTSI2SD
*   Shuffle: PSHUFD (basic lane-swap implementation)
*   Shift:   PSLLDQ, PSRLDQ (byte-granularity XMM shifts)
*   Other:   MOVDDUP, MOVSHDUP, MOVSLDUP
*
* Strategy: XMM registers are 128-bit integers in the LLVM IR. We bitcast
* them to <4 x float>, <2 x double>, <16 x i8>, etc. as needed, perform the
* vector operation, then bitcast the result back to i128 for storage.
*
* Wire-up: after adding this file to CMakeLists.txt, replace all the `nullptr`
* entries in x86_init.cpp with the appropriate function pointers from x86_impl.h.
* The apply script does this automatically via sed.
*/

#include <capstone/capstone.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/Intrinsics.h>
#include <llvm/IR/IntrinsicsX86.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Type.h>
#include <llvm/IR/Value.h>

// Include the impl header which declares all the translate* functions we define here.
// (The actual class is Capstone2LlvmIrTranslatorX86_impl.)
#include <cmath>
#include <cstring>
#include <string>
#include "x86_impl.h"

namespace retdec {
namespace capstone2llvmir {

using namespace llvm;

//===========================================================================
// Helpers
//===========================================================================

/// Return the 128-bit integer type used for XMM registers.
static Type* xmmTy(IRBuilder<>& irb) {
    return irb.getInt128Ty();
}

/// Bitcast a 128-bit XMM value to a vector type.
static Value* asVec(Value* v, Type* vecTy, IRBuilder<>& irb) {
    return irb.CreateBitCast(v, vecTy);
}

/// Bitcast a vector value back to i128 for XMM storage.
static Value* asXmm(Value* v, IRBuilder<>& irb) {
    return irb.CreateBitCast(v, irb.getInt128Ty());
}

/// Build <N x ElemTy> LLVM vector type.
static VectorType* vecType(Type* elem, unsigned n) {
    return FixedVectorType::get(elem, n);
}

static Value* toI128(Value* v, IRBuilder<>& irb);
static llvm::Value* extractLane128(llvm::Value* v, unsigned lane, llvm::IRBuilder<>& irb);
static llvm::Value* concatLane128(llvm::Value* lo, llvm::Value* hi, llvm::IRBuilder<>& irb);
static Value* movddupI128(Value* src, IRBuilder<>& irb);

//===========================================================================
// SSE Move Instructions
//===========================================================================

/**
 * MOVAPS, MOVAPD, MOVDQA, MOVDQU, VMOVAPS, VMOVAPD
 * Simple 128-bit register↔register or register↔memory move.
 * Alignment semantics are irrelevant at the IR level — all become a plain
 * 128-bit load or store.
 */
void Capstone2LlvmIrTranslatorX86_impl::translateSseMovWhole(
        cs_insn* i, cs_x86* xi, IRBuilder<>& irb) {
    EXPECT_IS_BINARY(i, xi, irb);
    auto* val = loadOpBinaryOp1(xi, irb);
    // Zero-extend if narrower than 128 bits (e.g. MOVD from 32-bit reg).
    if (val->getType()->isIntegerTy()
            && val->getType()->getIntegerBitWidth() < 128) {
        val = irb.CreateZExt(val, irb.getInt128Ty());
    }
    // ZEXT_TRUNC_OR_BITCAST lets storeRegister reconcile any remaining size
    // difference between the produced i128 and the XMM backing alloca.
    storeOp(xi->operands[0], val, irb, eOpConv::ZEXT_TRUNC_OR_BITCAST);
}

/**
 * MOVD / MOVQ — move between XMM and GPR/memory (lane 0 only).
 * MOVD: 32-bit lane 0 ↔ XMM (zero-extended to 128 bits on write).
 * MOVQ: 64-bit lane 0 ↔ XMM.
 *
 * Bug fixed: the original code used eOpConv::NOTHING for both directions.
 * When reading from XMM, the value is truncated to i32/i64 but the
 * destination GPR alloca is i64, so storeRegister emitted a store of
 * i32 to i64* which triggers StoreInst::AssertOK() and aborts. Using
 * ZEXT_TRUNC_OR_BITCAST lets storeRegister zero-extend as needed.
 */
void Capstone2LlvmIrTranslatorX86_impl::translateSseMovLane0(
        cs_insn* i, cs_x86* xi, IRBuilder<>& irb) {
    EXPECT_IS_BINARY(i, xi, irb);
    auto* src = loadOpBinaryOp1(xi, irb);
    if (src->getType()->isIntegerTy()) {
        // Writing to XMM: zero-extend to 128 bits, then store.
        if (src->getType()->getIntegerBitWidth() < 128)
            src = irb.CreateZExt(src, irb.getInt128Ty());
        storeOp(xi->operands[0], src, irb, eOpConv::ZEXT_TRUNC_OR_BITCAST);
    } else {
        // Reading from XMM: truncate to destination width, then store.
        // ZEXT_TRUNC_OR_BITCAST handles the case where the destination GPR
        // alloca (i64) is wider than the truncated value (i32).
        unsigned destBits = (i->id == X86_INS_MOVQ) ? 64 : 32;
        src = irb.CreateTrunc(src, irb.getIntNTy(destBits));
        storeOp(xi->operands[0], src, irb, eOpConv::ZEXT_TRUNC_OR_BITCAST);
    }
}

/**
 * MOVLHPS — copy lower 64 bits of src into upper 64 bits of dst.
 */
void Capstone2LlvmIrTranslatorX86_impl::translateMovLhps(
        cs_insn* i, cs_x86* xi, IRBuilder<>& irb) {
    if (i->id == X86_INS_VMOVLHPS)
    {
        if (xi->op_count != 3)
        {
            translatePseudoAsmGeneric(i, xi, irb);
            return;
        }
        Value* a = loadOp(xi->operands[1], irb);
        Value* b = loadOp(xi->operands[2], irb);
        Value* lo64 = irb.CreateTrunc(b, irb.getInt64Ty());
        Value* shifted = irb.CreateShl(irb.CreateZExt(lo64, irb.getInt128Ty()), ConstantInt::get(irb.getInt128Ty(), 64));
        Value* mask = irb.CreateZExt(ConstantInt::get(irb.getInt64Ty(), ~uint64_t(0)), irb.getInt128Ty());
        storeVectorOp(xi->operands[0], irb.CreateOr(irb.CreateAnd(a, mask), shifted), 128, irb);
        return;
    }
    EXPECT_IS_BINARY(i, xi, irb);
    std::tie(op0, op1) = loadOpBinary(xi, irb, eOpConv::NOTHING);
    // Take lower 64 of src.
    Value* lo64 = irb.CreateTrunc(op1, irb.getInt64Ty());
    // Shift into upper 64 bits of dst.
    Value* lo128 = irb.CreateZExt(lo64, irb.getInt128Ty());
    Value* shifted = irb.CreateShl(lo128, ConstantInt::get(irb.getInt128Ty(), 64));
    // Mask: keep lower 64 bits of dst intact (0x0000...FFFF...FFFF in i128).
    // Use ZExt from i64 all-ones — avoids APInt string-parse bugs with "0x" prefix.
    Value* mask = irb.CreateZExt(
        ConstantInt::get(irb.getInt64Ty(), ~uint64_t(0)), irb.getInt128Ty());
    Value* result = irb.CreateOr(irb.CreateAnd(op0, mask), shifted);
    storeOp(xi->operands[0], result, irb, eOpConv::ZEXT_TRUNC_OR_BITCAST);
}

/**
 * MOVHLPS — copy upper 64 bits of src into lower 64 bits of dst.
 */
void Capstone2LlvmIrTranslatorX86_impl::translateMovHlps(
        cs_insn* i, cs_x86* xi, IRBuilder<>& irb) {
    if (i->id == X86_INS_VMOVHLPS)
    {
        if (xi->op_count != 3)
        {
            translatePseudoAsmGeneric(i, xi, irb);
            return;
        }
        Value* a = loadOp(xi->operands[1], irb);
        Value* b = loadOp(xi->operands[2], irb);
        Value* hi64 = irb.CreateTrunc(irb.CreateLShr(b, ConstantInt::get(irb.getInt128Ty(), 64)), irb.getInt64Ty());
        Value* lo128 = irb.CreateZExt(hi64, irb.getInt128Ty());
        Value* loMask = irb.CreateZExt(ConstantInt::get(irb.getInt64Ty(), ~uint64_t(0)), irb.getInt128Ty());
        Value* hiMask = irb.CreateShl(loMask, ConstantInt::get(irb.getInt128Ty(), 64));
        storeVectorOp(xi->operands[0], irb.CreateOr(irb.CreateAnd(a, hiMask), lo128), 128, irb);
        return;
    }
    EXPECT_IS_BINARY(i, xi, irb);
    std::tie(op0, op1) = loadOpBinary(xi, irb, eOpConv::NOTHING);
    // Take upper 64 of src.
    Value* hi64 = irb.CreateTrunc(irb.CreateLShr(op1,
        ConstantInt::get(irb.getInt128Ty(), 64)), irb.getInt64Ty());
    Value* lo128 = irb.CreateZExt(hi64, irb.getInt128Ty());
    // Mask: keep upper 64 bits of dst intact (0xFFFF...FFFF0000...0000 in i128).
    Value* loMask = irb.CreateZExt(
        ConstantInt::get(irb.getInt64Ty(), ~uint64_t(0)), irb.getInt128Ty());
    Value* hiMask = irb.CreateShl(loMask, ConstantInt::get(irb.getInt128Ty(), 64));
    Value* result = irb.CreateOr(irb.CreateAnd(op0, hiMask), lo128);
    storeOp(xi->operands[0], result, irb, eOpConv::ZEXT_TRUNC_OR_BITCAST);
}

/**
 * MOVDDUP / VMOVDDUP — duplicate the low qword of each 128-bit lane.
 *
 * SSE MOVDDUP is dest, src. AVX VMOVDDUP is the same two-operand form, but a
 * VEX 128-bit write zeroes the upper half of YMM, and the YMM encoding
 * duplicates each 128-bit half independently (not the low qword of the whole
 * 256-bit register). The memory form of the XMM encoding reads 64 bits.
 */
void Capstone2LlvmIrTranslatorX86_impl::translateMovDdup(
        cs_insn* i, cs_x86* xi, IRBuilder<>& irb) {
    EXPECT_IS_BINARY(i, xi, irb);
    if (i->id != X86_INS_VMOVDDUP)
    {
        auto* src = loadOpBinaryOp1(xi, irb);
        storeOp(xi->operands[0], movddupI128(src, irb), irb, eOpConv::NOTHING);
        return;
    }

    unsigned destBits = xi->operands[0].size * 8;
    if (destBits != 128 && destBits != 256)
    {
        translatePseudoAsmGeneric(i, xi, irb);
        return;
    }

    unsigned srcBits = xi->operands[1].size * 8;
    Value* src = loadVectorOp(xi->operands[1], irb, srcBits);
    if (src == nullptr)
    {
        translatePseudoAsmGeneric(i, xi, irb);
        return;
    }

    Value* result = nullptr;
    if (destBits == 128)
    {
        result = movddupI128(src, irb);
    }
    else
    {
        src = irb.CreateZExtOrTrunc(src, irb.getIntNTy(256));
        result = concatLane128(
            movddupI128(extractLane128(src, 0, irb), irb),
            movddupI128(extractLane128(src, 1, irb), irb),
            irb);
    }
    storeVectorOp(xi->operands[0], result, destBits, irb);
}

/**
 * MOVSHDUP — duplicate odd (high) 32-bit floats: src[1]→[0,1], src[3]→[2,3].
 * MOVSLDUP — duplicate even (low) 32-bit floats: src[0]→[0,1], src[2]→[2,3].
 */
void Capstone2LlvmIrTranslatorX86_impl::translateMovShDup(
        cs_insn* i, cs_x86* xi, IRBuilder<>& irb) {
    EXPECT_IS_BINARY(i, xi, irb);
    bool isHigh = (i->id == X86_INS_MOVSHDUP || i->id == X86_INS_VMOVSHDUP);
    unsigned lane0 = isHigh ? 1 : 0;
    unsigned lane1 = isHigh ? 3 : 2;
    std::vector<int> mask = {
        static_cast<int>(lane0), static_cast<int>(lane0), static_cast<int>(lane1), static_cast<int>(lane1)};

    if (i->id == X86_INS_VMOVSHDUP || i->id == X86_INS_VMOVSLDUP)
    {
        unsigned destBits = xi->operands[0].size * 8;
        if (destBits != 128 && destBits != 256)
        {
            translatePseudoAsmGeneric(i, xi, irb);
            return;
        }
        unsigned srcBits = destBits;
        Value* src = loadVectorOp(xi->operands[1], irb, srcBits);
        if (src == nullptr)
        {
            translatePseudoAsmGeneric(i, xi, irb);
            return;
        }
        auto shuf128 = [&](Value* lane) {
            auto* vec4f = vecType(irb.getFloatTy(), 4);
            Value* v = irb.CreateBitCast(irb.CreateZExtOrTrunc(lane, irb.getInt128Ty()), vec4f);
            return irb.CreateBitCast(irb.CreateShuffleVector(v, v, mask), irb.getInt128Ty());
        };
        if (destBits == 128)
        {
            storeVectorOp(xi->operands[0], shuf128(src), 128, irb);
            return;
        }
        src = irb.CreateZExtOrTrunc(src, irb.getIntNTy(256));
        storeVectorOp(
            xi->operands[0],
            concatLane128(shuf128(extractLane128(src, 0, irb)), shuf128(extractLane128(src, 1, irb)), irb),
            256,
            irb);
        return;
    }

    auto* src = loadOpBinaryOp1(xi, irb);
    auto* vec4f = vecType(irb.getFloatTy(), 4);
    auto* v = asVec(src, vec4f, irb);
    Value* shuf = irb.CreateShuffleVector(v, v, mask);
    storeOp(xi->operands[0], asXmm(shuf, irb), irb, eOpConv::NOTHING);
}

//===========================================================================
// SSE Integer Arithmetic
//===========================================================================

/// Ensure \p v is exactly 128 bits (zero-extend if narrower, truncate if wider).
/// XMM registers are always i128 in our IR; memory operands may be narrower.
static Value* toI128(Value* v, IRBuilder<>& irb) {
    auto* i128 = irb.getInt128Ty();
    Type* ty = v->getType();
    if (ty == i128) return v;
    unsigned bits = ty->getPrimitiveSizeInBits();
    if (bits < 128) return irb.CreateZExt(v, i128);
    return irb.CreateTrunc(v, i128);
}

/// Low or high 128-bit half of a 256-bit value. lane 0 is bits 127:0.
static llvm::Value* extractLane128(llvm::Value* v, unsigned lane, llvm::IRBuilder<>& irb)
{
	auto* i128 = irb.getInt128Ty();
	auto* i256 = irb.getIntNTy(256);
	v = irb.CreateZExtOrTrunc(v, i256);
	if (lane == 0)
	{
		return irb.CreateTrunc(v, i128);
	}
	return irb.CreateTrunc(irb.CreateLShr(v, llvm::ConstantInt::get(i256, 128)), i128);
}

static llvm::Value* concatLane128(llvm::Value* lo, llvm::Value* hi, llvm::IRBuilder<>& irb)
{
	auto* i128 = irb.getInt128Ty();
	auto* i256 = irb.getIntNTy(256);
	lo = irb.CreateZExt(irb.CreateZExtOrTrunc(lo, i128), i256);
	hi = irb.CreateZExt(irb.CreateZExtOrTrunc(hi, i128), i256);
	return irb.CreateOr(lo, irb.CreateShl(hi, llvm::ConstantInt::get(i256, 128)));
}

/// PUNPCKL/H of one 128-bit pair. AVX2 repeats this independently per lane.
static Value* punpckI128(Value* a, Value* b, unsigned bits, bool high, IRBuilder<>& irb)
{
	unsigned lanes = 128 / bits;
	auto* vecTy = FixedVectorType::get(irb.getIntNTy(bits), lanes);
	Value* v0 = irb.CreateBitCast(toI128(a, irb), vecTy);
	Value* v1 = irb.CreateBitCast(toI128(b, irb), vecTy);
	unsigned halfLanes = lanes / 2;
	unsigned base = high ? halfLanes : 0;
	std::vector<int> mask;
	mask.reserve(lanes);
	for (unsigned n = 0; n < halfLanes; ++n)
	{
		mask.push_back(static_cast<int>(base + n));
		mask.push_back(static_cast<int>(lanes + base + n));
	}
	return irb.CreateBitCast(irb.CreateShuffleVector(v0, v1, mask), irb.getInt128Ty());
}

/// PSHUFD of one 128-bit value. AVX2 applies the same imm8 to each lane.
static Value* pshufdI128(Value* src, uint8_t ctrl, IRBuilder<>& irb)
{
	auto* vec4i = FixedVectorType::get(irb.getInt32Ty(), 4);
	Value* v = irb.CreateBitCast(toI128(src, irb), vec4i);
	std::vector<int> mask = {
		static_cast<int>((ctrl >> 0) & 3),
		static_cast<int>((ctrl >> 2) & 3),
		static_cast<int>((ctrl >> 4) & 3),
		static_cast<int>((ctrl >> 6) & 3)};
	return irb.CreateBitCast(irb.CreateShuffleVector(v, v, mask), irb.getInt128Ty());
}

/// PSHUFHW / PSHUFLW of one 128-bit value. `first` is the first word touched
/// (4 for HW, 0 for LW). AVX2 repeats this independently per 128-bit lane.
static Value* pshufhwlwI128(Value* src, uint64_t imm, unsigned first, IRBuilder<>& irb)
{
	auto* vecTy = FixedVectorType::get(irb.getInt16Ty(), 8);
	Value* v = irb.CreateBitCast(toI128(src, irb), vecTy);
	llvm::SmallVector<int, 8> mask;
	for (unsigned lane = 0; lane < 8; ++lane)
	{
		if (lane < first || lane >= first + 4)
		{
			mask.push_back(static_cast<int>(lane));
		}
		else
		{
			unsigned sel = (imm >> (2 * (lane - first))) & 3;
			mask.push_back(static_cast<int>(first + sel));
		}
	}
	return irb.CreateBitCast(irb.CreateShuffleVector(v, llvm::UndefValue::get(vecTy), mask), irb.getInt128Ty());
}

/// Duplicate the low qword of a 128-bit value into both 64-bit lanes.
static Value* movddupI128(Value* src, IRBuilder<>& irb)
{
	Value* lo64 = irb.CreateTrunc(toI128(src, irb), irb.getInt64Ty());
	Value* lo128 = irb.CreateZExt(lo64, irb.getInt128Ty());
	return irb.CreateOr(lo128, irb.CreateShl(lo128, ConstantInt::get(irb.getInt128Ty(), 64)));
}

/// Generic packed integer binary op helper.
static Value* packedIntBinOp(
        Value* op0, Value* op1,
        unsigned elemBits, unsigned numElems,
        unsigned opcode,   // llvm::Instruction::Add etc.
        IRBuilder<>& irb) {
    auto* vecTy = FixedVectorType::get(irb.getIntNTy(elemBits), numElems);
    Value* v0 = irb.CreateBitCast(toI128(op0, irb), vecTy);
    Value* v1 = irb.CreateBitCast(toI128(op1, irb), vecTy);
    Value* result = irb.CreateBinOp(
        static_cast<Instruction::BinaryOps>(opcode), v0, v1);
    return irb.CreateBitCast(result, irb.getInt128Ty());
}

/**
 * PADDB/W/D/Q — packed integer add (8/16/32/64-bit lanes).
 */
void Capstone2LlvmIrTranslatorX86_impl::translateSsePadd(
        cs_insn* i, cs_x86* xi, IRBuilder<>& irb) {
    EXPECT_IS_BINARY(i, xi, irb);
    std::tie(op0, op1) = loadOpBinary(xi, irb, eOpConv::NOTHING);

    unsigned bits;
    switch (i->id) {
        case X86_INS_PADDB: bits =  8; break;
        case X86_INS_PADDW: bits = 16; break;
        case X86_INS_PADDD: bits = 32; break;
        case X86_INS_PADDQ: bits = 64; break;
        default: bits = 32; break;
    }
    auto* res = packedIntBinOp(op0, op1, bits, 128 / bits,
                                Instruction::Add, irb);
    storeOp(xi->operands[0], res, irb, eOpConv::NOTHING);
}

/**
 * PSUBB/W/D/Q — packed integer subtract.
 */
void Capstone2LlvmIrTranslatorX86_impl::translateSsePsub(
        cs_insn* i, cs_x86* xi, IRBuilder<>& irb) {
    EXPECT_IS_BINARY(i, xi, irb);
    std::tie(op0, op1) = loadOpBinary(xi, irb, eOpConv::NOTHING);

    unsigned bits;
    switch (i->id) {
        case X86_INS_PSUBB: bits =  8; break;
        case X86_INS_PSUBW: bits = 16; break;
        case X86_INS_PSUBD: bits = 32; break;
        case X86_INS_PSUBQ: bits = 64; break;
        default: bits = 32; break;
    }
    auto* res = packedIntBinOp(op0, op1, bits, 128 / bits,
                                Instruction::Sub, irb);
    storeOp(xi->operands[0], res, irb, eOpConv::NOTHING);
}

/**
 * PAND — bitwise AND of 128-bit XMM values.
 */
void Capstone2LlvmIrTranslatorX86_impl::translateSsePand(
        cs_insn* i, cs_x86* xi, IRBuilder<>& irb) {
    EXPECT_IS_BINARY(i, xi, irb);
    std::tie(op0, op1) = loadOpBinary(xi, irb, eOpConv::NOTHING);
    storeOp(xi->operands[0], irb.CreateAnd(op0, op1), irb, eOpConv::NOTHING);
}

/**
 * PANDN — bitwise AND-NOT: dst = ~dst & src.
 */
void Capstone2LlvmIrTranslatorX86_impl::translateSsePandn(
        cs_insn* i, cs_x86* xi, IRBuilder<>& irb) {
    EXPECT_IS_BINARY(i, xi, irb);
    std::tie(op0, op1) = loadOpBinary(xi, irb, eOpConv::NOTHING);
    storeOp(xi->operands[0], irb.CreateAnd(irb.CreateNot(op0), op1),
            irb, eOpConv::NOTHING);
}

/**
 * POR — bitwise OR.
 */
void Capstone2LlvmIrTranslatorX86_impl::translateSsePor(
        cs_insn* i, cs_x86* xi, IRBuilder<>& irb) {
    EXPECT_IS_BINARY(i, xi, irb);
    std::tie(op0, op1) = loadOpBinary(xi, irb, eOpConv::NOTHING);
    storeOp(xi->operands[0], irb.CreateOr(op0, op1), irb, eOpConv::NOTHING);
}

/**
 * PXOR — bitwise XOR. Also used as "zero XMM" idiom when src==dst.
 */
void Capstone2LlvmIrTranslatorX86_impl::translateSsePxor(
        cs_insn* i, cs_x86* xi, IRBuilder<>& irb) {
    EXPECT_IS_BINARY(i, xi, irb);
    std::tie(op0, op1) = loadOpBinary(xi, irb, eOpConv::NOTHING);
    storeOp(xi->operands[0], irb.CreateXor(op0, op1), irb, eOpConv::NOTHING);
}

/**
 * PCMPEQB/W/D/Q, PCMPGTB/W/D/Q — packed compare; a result lane is all-1s or
 * all-0s.
 *
 * The two families are one function because they differ in exactly one
 * predicate, and splitting them is how the lane width ends up spelled twice.
 * The comparison for PCMPGT is **signed** -- that is the whole content of the
 * instruction, and the unsigned reading gives a different answer for every
 * lane with its top bit set, which for byte lanes is half of them.
 *
 * `laneBits` throws on an id it does not know rather than defaulting to 32.
 * The defaulting version was live here: PCMPEQQ pointed at `nullptr`, and
 * wiring it to this function would have silently compared 32-bit lanes.
 */
static unsigned packedCmpLaneBits(unsigned id)
{
	switch (id)
	{
	case X86_INS_PCMPEQB:
	case X86_INS_PCMPGTB: return 8;
	case X86_INS_PCMPEQW:
	case X86_INS_PCMPGTW: return 16;
	case X86_INS_PCMPEQD:
	case X86_INS_PCMPGTD: return 32;
	case X86_INS_PCMPEQQ:
	case X86_INS_PCMPGTQ: return 64;
	default: throw GenericError("packedCmpLaneBits(): unhandled instruction id");
	}
}

void Capstone2LlvmIrTranslatorX86_impl::translateSsePcmpeq(
        cs_insn* i, cs_x86* xi, IRBuilder<>& irb) {
    EXPECT_IS_BINARY(i, xi, irb);
    std::tie(op0, op1) = loadOpBinary(xi, irb, eOpConv::NOTHING);

	bool isGt =
		i->id == X86_INS_PCMPGTB || i->id == X86_INS_PCMPGTW || i->id == X86_INS_PCMPGTD || i->id == X86_INS_PCMPGTQ;
	unsigned bits = packedCmpLaneBits(i->id);
	unsigned lanes = 128 / bits;
	auto* vecTy = FixedVectorType::get(irb.getIntNTy(bits), lanes);
	Value* v0 = irb.CreateBitCast(toI128(op0, irb), vecTy);
	Value* v1 = irb.CreateBitCast(toI128(op1, irb), vecTy);
    // i1 vector comparison.
	Value* cmp = isGt ? irb.CreateICmpSGT(v0, v1) : irb.CreateICmpEQ(v0, v1);
	// Sign-extend i1 → iN to get 0xFF…F or 0x00…0.
	Value* ext = irb.CreateSExt(cmp, vecTy);
	storeOp(xi->operands[0], irb.CreateBitCast(ext, irb.getInt128Ty()), irb, eOpConv::NOTHING);
}

/**
 * PUNPCKL{BW,WD,DQ,QDQ} and PUNPCKH{BW,WD,DQ,QDQ} — unpack and interleave.
 *
 * This used to read `unsigned bits = (i->id == X86_INS_PUNPCKLBW) ? 8 : 32;`
 * and was correct because exactly two ids reached it, PUNPCKLBW and
 * PUNPCKLDQ. It is the kind of correct that stops being correct the moment
 * someone adds a third entry to the table: PUNPCKLWD pointed at `nullptr`
 * next to those two, and wiring it there would have interleaved 32-bit lanes
 * under a 16-bit mnemonic -- no crash, no assertion, a different answer.
 *
 * The high forms take the top half of each operand instead of the bottom, and
 * are what a compiler uses to widen the upper lanes of a vector. They are the
 * same shuffle with the source indices moved up by half the lane count.
 */
void Capstone2LlvmIrTranslatorX86_impl::translateSsePunpckl(
        cs_insn* i, cs_x86* xi, IRBuilder<>& irb) {
	unsigned bits = 0;
	bool high = false;
	bool vex = false;
	switch (i->id)
	{
	case X86_INS_VPUNPCKLBW:
		vex = true;
		// fall through
	case X86_INS_PUNPCKLBW: bits = 8; break;
	case X86_INS_VPUNPCKLWD:
		vex = true;
		// fall through
	case X86_INS_PUNPCKLWD: bits = 16; break;
	case X86_INS_VPUNPCKLDQ:
		vex = true;
		// fall through
	case X86_INS_PUNPCKLDQ: bits = 32; break;
	case X86_INS_VPUNPCKLQDQ:
		vex = true;
		// fall through
	case X86_INS_PUNPCKLQDQ: bits = 64; break;
	case X86_INS_VPUNPCKHBW:
		vex = true;
		// fall through
	case X86_INS_PUNPCKHBW:
		bits = 8;
		high = true;
		break;
	case X86_INS_VPUNPCKHWD:
		vex = true;
		// fall through
	case X86_INS_PUNPCKHWD:
		bits = 16;
		high = true;
		break;
	case X86_INS_VPUNPCKHDQ:
		vex = true;
		// fall through
	case X86_INS_PUNPCKHDQ:
		bits = 32;
		high = true;
		break;
	case X86_INS_VPUNPCKHQDQ:
		vex = true;
		// fall through
	case X86_INS_PUNPCKHQDQ:
		bits = 64;
		high = true;
		break;
	default: throw GenericError("translateSsePunpckl(): unhandled instruction id");
	}

	if (vex)
	{
		if (xi->op_count != 3)
		{
			translatePseudoAsmGeneric(i, xi, irb);
			return;
		}
		unsigned destBits = xi->operands[0].size * 8;
		if (destBits != 128 && destBits != 256)
		{
			translatePseudoAsmGeneric(i, xi, irb);
			return;
		}
		Value* a = loadVectorOp(xi->operands[1], irb, destBits);
		Value* b = loadVectorOp(xi->operands[2], irb, destBits);
		if (a == nullptr || b == nullptr)
		{
			translatePseudoAsmGeneric(i, xi, irb);
			return;
		}
		Value* result = destBits == 128
			? punpckI128(a, b, bits, high, irb)
			: concatLane128(
				punpckI128(extractLane128(a, 0, irb), extractLane128(b, 0, irb), bits, high, irb),
				punpckI128(extractLane128(a, 1, irb), extractLane128(b, 1, irb), bits, high, irb),
				irb);
		storeVectorOp(xi->operands[0], result, destBits, irb);
		return;
	}

    EXPECT_IS_BINARY(i, xi, irb);
    std::tie(op0, op1) = loadOpBinary(xi, irb, eOpConv::NOTHING);
	storeOp(xi->operands[0], punpckI128(op0, op1, bits, high, irb), irb, eOpConv::NOTHING);
}

/**
 * PSHUFD — shuffle 32-bit lanes of XMM by immediate control byte.
 * dst[i] = src[ (imm >> (i*2)) & 3 ]
 */
void Capstone2LlvmIrTranslatorX86_impl::translateSsePshufd(
        cs_insn* i, cs_x86* xi, IRBuilder<>& irb) {
    EXPECT_IS_TERNARY(i, xi, irb);
	if (xi->operands[2].type != X86_OP_IMM)
	{
		translatePseudoAsmGeneric(i, xi, irb);
		return;
	}
	uint8_t ctrl = static_cast<uint8_t>(xi->operands[2].imm);
	if (i->id == X86_INS_PSHUFW)
	{
		auto* i64 = irb.getInt64Ty();
		auto* vec4w = FixedVectorType::get(irb.getInt16Ty(), 4);
		Value* src = irb.CreateBitCast(irb.CreateZExtOrTrunc(loadOp(xi->operands[1], irb), i64), vec4w);
		std::vector<int> mask = {
			static_cast<int>((ctrl >> 0) & 3),
			static_cast<int>((ctrl >> 2) & 3),
			static_cast<int>((ctrl >> 4) & 3),
			static_cast<int>((ctrl >> 6) & 3)};
		storeOp(
			xi->operands[0],
			irb.CreateBitCast(irb.CreateShuffleVector(src, src, mask), i64),
			irb,
			eOpConv::ZEXT_TRUNC_OR_BITCAST);
		return;
	}
	bool vex = (i->id == X86_INS_VPSHUFD);
	if (!vex)
	{
		storeOp(xi->operands[0], pshufdI128(loadOp(xi->operands[1], irb), ctrl, irb), irb, eOpConv::NOTHING);
		return;
	}

	unsigned destBits = xi->operands[0].size * 8;
	if (destBits != 128 && destBits != 256)
	{
		translatePseudoAsmGeneric(i, xi, irb);
		return;
	}
	Value* src = loadVectorOp(xi->operands[1], irb, destBits);
	if (src == nullptr)
	{
		translatePseudoAsmGeneric(i, xi, irb);
		return;
	}
	Value* result = destBits == 128
		? pshufdI128(src, ctrl, irb)
		: concatLane128(
			pshufdI128(extractLane128(src, 0, irb), ctrl, irb),
			pshufdI128(extractLane128(src, 1, irb), ctrl, irb),
			irb);
	storeVectorOp(xi->operands[0], result, destBits, irb);
}

/**
 * PSLLDQ — shift XMM left by N bytes (zero-fill from right).
 * PSRLDQ — shift XMM right by N bytes (zero-fill from left).
 */
/// PSLLDQ / PSRLDQ of one 128-bit lane. Counts of 16 bytes or more are
/// architecturally zero, not a wrapped shift of the whole register.
static Value* pbyteShiftI128(Value* src, uint64_t bytes, bool left, IRBuilder<>& irb)
{
	auto* i128 = irb.getInt128Ty();
	src = toI128(src, irb);
	uint64_t bits = bytes * 8;
	if (bits >= 128)
	{
		return ConstantInt::get(i128, 0);
	}
	Value* amt = ConstantInt::get(i128, bits);
	return left ? irb.CreateShl(src, amt) : irb.CreateLShr(src, amt);
}

void Capstone2LlvmIrTranslatorX86_impl::translateSsePbyteShift(
        cs_insn* i, cs_x86* xi, IRBuilder<>& irb) {
	bool vex = (i->id == X86_INS_VPSLLDQ || i->id == X86_INS_VPSRLDQ);
	bool left = (i->id == X86_INS_PSLLDQ || i->id == X86_INS_VPSLLDQ);
	unsigned srcOp = 0;
	unsigned immOp = 1;
	if (vex)
	{
		// Capstone: vpslldq xmm0, xmm1, 1 is dest, src, imm -- three
		// operands. SSE PSLLDQ is dest/src, imm.
		if (xi->op_count != 3 || xi->operands[2].type != X86_OP_IMM)
		{
			translatePseudoAsmGeneric(i, xi, irb);
			return;
		}
		srcOp = 1;
		immOp = 2;
	}
	else
	{
		EXPECT_IS_BINARY(i, xi, irb);
	}

	uint64_t bytes = static_cast<uint64_t>(xi->operands[immOp].imm);

	if (!vex)
	{
		auto* src = loadOpBinaryOp0(xi, irb);
		storeOp(xi->operands[0], pbyteShiftI128(src, bytes, left, irb), irb, eOpConv::NOTHING);
		return;
	}

	unsigned bits = xi->operands[0].size * 8;
	if (bits != 128 && bits != 256)
	{
		translatePseudoAsmGeneric(i, xi, irb);
		return;
	}

	Value* src = loadVectorOp(xi->operands[srcOp], irb, bits);
	if (src == nullptr)
	{
		translatePseudoAsmGeneric(i, xi, irb);
		return;
	}

	Value* result = nullptr;
	if (bits == 128)
	{
		result = pbyteShiftI128(src, bytes, left, irb);
	}
	else
	{
		// AVX2 VPSLLDQ/VPSRLDQ shift each 128-bit half independently. A
		// single i256 shift would move bytes across the lane boundary.
		auto* i256 = irb.getIntNTy(256);
		src = irb.CreateZExtOrTrunc(src, i256);
		Value* lo = pbyteShiftI128(irb.CreateTrunc(src, irb.getInt128Ty()), bytes, left, irb);
		Value* hi = pbyteShiftI128(
			irb.CreateTrunc(irb.CreateLShr(src, ConstantInt::get(i256, 128)), irb.getInt128Ty()),
			bytes,
			left,
			irb);
		result = irb.CreateOr(
			irb.CreateZExt(lo, i256),
			irb.CreateShl(irb.CreateZExt(hi, i256), ConstantInt::get(i256, 128)));
	}
	storeVectorOp(xi->operands[0], result, bits, irb);
}

//===========================================================================
// SSE Float Arithmetic
//===========================================================================

/// Generic packed float binary op (4 x float or 2 x double).
static Value* packedFltBinOp(
        Value* op0, Value* op1,
        bool isDouble,
        unsigned opcode,
        IRBuilder<>& irb) {
    Type* elemTy = isDouble ? irb.getDoubleTy() : irb.getFloatTy();
    unsigned n   = isDouble ? 2 : 4;
    auto* vecTy  = FixedVectorType::get(elemTy, n);
    Value* v0    = irb.CreateBitCast(toI128(op0, irb), vecTy);
    Value* v1    = irb.CreateBitCast(toI128(op1, irb), vecTy);
    Value* res   = irb.CreateBinOp(
                       static_cast<Instruction::BinaryOps>(opcode), v0, v1);
    return irb.CreateBitCast(res, irb.getInt128Ty());
}

/// Scalar float op (lowest lane only, upper lanes preserved from dst).
static Value* scalarFltBinOp(
        Value* dst, Value* src,
        bool isDouble,
        unsigned opcode,
        IRBuilder<>& irb) {
    Type* elemTy = isDouble ? irb.getDoubleTy() : irb.getFloatTy();
    unsigned n   = isDouble ? 2 : 4;
    auto* vecTy  = FixedVectorType::get(elemTy, n);
    // For scalar ops, src lane 0 can be extracted from a narrower value
    // (e.g. a 32-bit float loaded from memory for MULSS).
    Value* i128Dst = toI128(dst, irb);
    Value* i128Src = toI128(src, irb);
    Value* vDst  = irb.CreateBitCast(i128Dst, vecTy);
    Value* vSrc  = irb.CreateBitCast(i128Src, vecTy);
    Value* e0Dst = irb.CreateExtractElement(vDst, (uint64_t)0);
    Value* e0Src = irb.CreateExtractElement(vSrc, (uint64_t)0);
    Value* result = irb.CreateBinOp(
                        static_cast<Instruction::BinaryOps>(opcode),
                        e0Dst, e0Src);
    Value* updated = irb.CreateInsertElement(vDst, result, (uint64_t)0);
    return irb.CreateBitCast(updated, irb.getInt128Ty());
}

/// One 128-bit HADD/HSUB of packed singles or doubles.
static Value* sseHaddHsubI128(Value* a, Value* b, bool isDouble, bool isAdd, IRBuilder<>& irb)
{
	a = toI128(a, irb);
	b = toI128(b, irb);
	if (isDouble)
	{
		auto* vec2d = vecType(irb.getDoubleTy(), 2);
		Value* v0 = irb.CreateBitCast(a, vec2d);
		Value* v1 = irb.CreateBitCast(b, vec2d);
		Value* e00 = irb.CreateExtractElement(v0, (uint64_t)0);
		Value* e01 = irb.CreateExtractElement(v0, (uint64_t)1);
		Value* e10 = irb.CreateExtractElement(v1, (uint64_t)0);
		Value* e11 = irb.CreateExtractElement(v1, (uint64_t)1);
		Value* res = UndefValue::get(vec2d);
		if (isAdd)
		{
			res = irb.CreateInsertElement(res, irb.CreateFAdd(e00, e01), (uint64_t)0);
			res = irb.CreateInsertElement(res, irb.CreateFAdd(e10, e11), (uint64_t)1);
		}
		else
		{
			res = irb.CreateInsertElement(res, irb.CreateFSub(e00, e01), (uint64_t)0);
			res = irb.CreateInsertElement(res, irb.CreateFSub(e10, e11), (uint64_t)1);
		}
		return irb.CreateBitCast(res, irb.getInt128Ty());
	}

	auto* vec4f = vecType(irb.getFloatTy(), 4);
	Value* v0 = irb.CreateBitCast(a, vec4f);
	Value* v1 = irb.CreateBitCast(b, vec4f);
	Value* e00 = irb.CreateExtractElement(v0, (uint64_t)0);
	Value* e01 = irb.CreateExtractElement(v0, (uint64_t)1);
	Value* e02 = irb.CreateExtractElement(v0, (uint64_t)2);
	Value* e03 = irb.CreateExtractElement(v0, (uint64_t)3);
	Value* e10 = irb.CreateExtractElement(v1, (uint64_t)0);
	Value* e11 = irb.CreateExtractElement(v1, (uint64_t)1);
	Value* e12 = irb.CreateExtractElement(v1, (uint64_t)2);
	Value* e13 = irb.CreateExtractElement(v1, (uint64_t)3);
	Value* res = UndefValue::get(vec4f);
	if (isAdd)
	{
		res = irb.CreateInsertElement(res, irb.CreateFAdd(e00, e01), (uint64_t)0);
		res = irb.CreateInsertElement(res, irb.CreateFAdd(e02, e03), (uint64_t)1);
		res = irb.CreateInsertElement(res, irb.CreateFAdd(e10, e11), (uint64_t)2);
		res = irb.CreateInsertElement(res, irb.CreateFAdd(e12, e13), (uint64_t)3);
	}
	else
	{
		res = irb.CreateInsertElement(res, irb.CreateFSub(e00, e01), (uint64_t)0);
		res = irb.CreateInsertElement(res, irb.CreateFSub(e02, e03), (uint64_t)1);
		res = irb.CreateInsertElement(res, irb.CreateFSub(e10, e11), (uint64_t)2);
		res = irb.CreateInsertElement(res, irb.CreateFSub(e12, e13), (uint64_t)3);
	}
	return irb.CreateBitCast(res, irb.getInt128Ty());
}

/**
 * ADDSUBPS — alternating add/sub: even lanes subtract, odd lanes add.
 * HADDPS — horizontal add: dst[0]=src1[0]+src1[1], dst[1]=src1[2]+src1[3],
 *           dst[2]=src2[0]+src2[1], dst[3]=src2[2]+src2[3].
 * HSUBPS — horizontal sub.
 * VHADDPD/PS, VHSUBPD/PS — the VEX twins; YMM is two independent 128-bit
 * horizontal ops, one per lane.
 *
 * The plain lane-wise forms this used to also carry -- ADDPS and ADDSS -- are
 * in translateSseFltArith with their SUB, MUL, DIV, PD and SD counterparts.
 */
void Capstone2LlvmIrTranslatorX86_impl::translateSseHorizontal(cs_insn* i, cs_x86* xi, IRBuilder<>& irb)
{
	bool vex = (i->id == X86_INS_VHADDPD || i->id == X86_INS_VHADDPS
		|| i->id == X86_INS_VHSUBPD || i->id == X86_INS_VHSUBPS
		|| i->id == X86_INS_VADDSUBPD || i->id == X86_INS_VADDSUBPS);
	bool isDouble = (i->id == X86_INS_HADDPD || i->id == X86_INS_HSUBPD
		|| i->id == X86_INS_VHADDPD || i->id == X86_INS_VHSUBPD
		|| i->id == X86_INS_ADDSUBPD || i->id == X86_INS_VADDSUBPD);
	bool isAdd = (i->id == X86_INS_HADDPS || i->id == X86_INS_HADDPD
		|| i->id == X86_INS_VHADDPS || i->id == X86_INS_VHADDPD);
	bool isAddSub = (i->id == X86_INS_ADDSUBPS || i->id == X86_INS_ADDSUBPD
		|| i->id == X86_INS_VADDSUBPS || i->id == X86_INS_VADDSUBPD);

	if (vex)
	{
		if (xi->op_count != 3)
		{
			translatePseudoAsmGeneric(i, xi, irb);
			return;
		}
		unsigned destBits = xi->operands[0].size * 8;
		if (destBits != 128 && destBits != 256)
		{
			translatePseudoAsmGeneric(i, xi, irb);
			return;
		}
		unsigned bits = destBits;
		Value* a = loadVectorOp(xi->operands[1], irb, bits);
		bits = destBits;
		Value* b = loadVectorOp(xi->operands[2], irb, bits);
		if (a == nullptr || b == nullptr)
		{
			translatePseudoAsmGeneric(i, xi, irb);
			return;
		}
		if (isAddSub)
		{
			Type* elemTy = isDouble ? irb.getDoubleTy() : irb.getFloatTy();
			unsigned n = destBits / (isDouble ? 64u : 32u);
			auto* vecTy = vecType(elemTy, n);
			auto* intTy = irb.getIntNTy(destBits);
			Value* v0 = irb.CreateBitCast(irb.CreateZExtOrTrunc(a, intTy), vecTy);
			Value* v1 = irb.CreateBitCast(irb.CreateZExtOrTrunc(b, intTy), vecTy);
			Value* adds = irb.CreateFAdd(v0, v1);
			Value* subs = irb.CreateFSub(v0, v1);
			std::vector<int> mask;
			mask.reserve(n);
			for (unsigned lane = 0; lane < n; ++lane)
			{
				mask.push_back(static_cast<int>((lane & 1u) ? (n + lane) : lane));
			}
			storeVectorOp(
				xi->operands[0],
				irb.CreateBitCast(irb.CreateShuffleVector(subs, adds, mask), intTy),
				destBits,
				irb);
			return;
		}
		Value* result = nullptr;
		if (destBits == 128)
		{
			result = sseHaddHsubI128(a, b, isDouble, isAdd, irb);
		}
		else
		{
			result = concatLane128(
				sseHaddHsubI128(extractLane128(a, 0, irb), extractLane128(b, 0, irb), isDouble, isAdd, irb),
				sseHaddHsubI128(extractLane128(a, 1, irb), extractLane128(b, 1, irb), isDouble, isAdd, irb),
				irb);
		}
		storeVectorOp(xi->operands[0], result, destBits, irb);
		return;
	}

	EXPECT_IS_BINARY(i, xi, irb);
	std::tie(op0, op1) = loadOpBinary(xi, irb, eOpConv::NOTHING);

	if (i->id == X86_INS_ADDSUBPS || i->id == X86_INS_ADDSUBPD)
	{
		// Even lanes subtract, odd lanes add (SDM ADDSUBP{S,D}).
		const bool isDouble = i->id == X86_INS_ADDSUBPD;
		Type* elemTy = isDouble ? irb.getDoubleTy() : irb.getFloatTy();
		unsigned n = isDouble ? 2u : 4u;
		auto* vecTy = vecType(elemTy, n);
		Value* v0 = irb.CreateBitCast(toI128(op0, irb), vecTy);
		Value* v1 = irb.CreateBitCast(toI128(op1, irb), vecTy);
		Value* adds = irb.CreateFAdd(v0, v1);
		Value* subs = irb.CreateFSub(v0, v1);
		// First vector is subs (indices 0..n-1), second is adds (n..2n-1).
		std::vector<int> mask;
		mask.reserve(n);
		for (unsigned lane = 0; lane < n; ++lane)
		{
			mask.push_back(static_cast<int>((lane & 1u) ? (n + lane) : lane));
		}
		Value* blended = irb.CreateShuffleVector(subs, adds, mask);
		storeOp(xi->operands[0], irb.CreateBitCast(blended, irb.getInt128Ty()),
				irb, eOpConv::NOTHING);
		return;
	}

	if (i->id == X86_INS_HADDPS || i->id == X86_INS_HSUBPS
		|| i->id == X86_INS_HADDPD || i->id == X86_INS_HSUBPD)
	{
		storeOp(xi->operands[0], sseHaddHsubI128(op0, op1, isDouble, isAdd, irb), irb, eOpConv::NOTHING);
	}
}

//===========================================================================
// SSE Convert Instructions
//===========================================================================

/**
 * CVTSI2SS / VCVTSI2SS — int32/64 → float32 (scalar, lane 0).
 */
void Capstone2LlvmIrTranslatorX86_impl::translateCvtSi2Ss(
        cs_insn* i, cs_x86* xi, IRBuilder<>& irb) {
    EXPECT_IS_BINARY(i, xi, irb);
    std::tie(op0, op1) = loadOpBinary(xi, irb, eOpConv::NOTHING);
    // op1 is the integer source; op0 is the destination XMM.
    Value* floatVal = irb.CreateSIToFP(op1, irb.getFloatTy());
    // Insert into lane 0 of dst.
    auto* vec4f = vecType(irb.getFloatTy(), 4);
    Value* vDst  = irb.CreateBitCast(toI128(op0, irb), vec4f);
    Value* updated = irb.CreateInsertElement(vDst, floatVal, (uint64_t)0);
    storeOp(xi->operands[0], irb.CreateBitCast(updated, irb.getInt128Ty()),
            irb, eOpConv::NOTHING);
}

/**
 * CVTSS2SI — float32 lane 0 → int32/64.
 *
 * This ROUNDS, to nearest-even under the default MXCSR; the truncating form is
 * CVTTSS2SI, and it is a separate instruction for a reason. The comment here
 * used to say "round toward nearest (C default); use FPToSI (truncation)",
 * which is two statements that contradict each other: FPToSI truncates, so
 * `cvtss2si eax, xmm0` on 2.7 answered 2 where the hardware answers 3.
 *
 * The destination width is the operand's, not the address size's:
 * `cvtss2si eax, xmm0` is a 32-bit conversion inside a 64-bit program.
 */
void Capstone2LlvmIrTranslatorX86_impl::translateCvtSs2Si(
        cs_insn* i, cs_x86* xi, IRBuilder<>& irb) {
    EXPECT_IS_BINARY(i, xi, irb);
    auto* src = loadOpBinaryOp1(xi, irb);
    auto* vec4f = vecType(irb.getFloatTy(), 4);
    Value* v = irb.CreateBitCast(toI128(src, irb), vec4f);
    Value* lane0 = irb.CreateExtractElement(v, (uint64_t)0);
	lane0 = irb.CreateUnaryIntrinsic(Intrinsic::roundeven, lane0);
	unsigned destBits = xi->operands[0].size ? xi->operands[0].size * 8 : 32;
	Value* intVal = generateFpToSiDefined(lane0, irb.getIntNTy(destBits), irb);
	// ZEXT_TRUNC_OR_BITCAST: the destination GPR alloca may be wider
	// (e.g. i64) than intVal (i32), which would crash StoreInst::AssertOK.
	storeOp(xi->operands[0], intVal, irb, eOpConv::ZEXT_TRUNC_OR_BITCAST);
}

/**
 * CVTSI2SD / VCVTSI2SD — int32/64 → float64 (scalar, lane 0).
 */
void Capstone2LlvmIrTranslatorX86_impl::translateCvtSi2Sd(
        cs_insn* i, cs_x86* xi, IRBuilder<>& irb) {
    EXPECT_IS_BINARY(i, xi, irb);
    std::tie(op0, op1) = loadOpBinary(xi, irb, eOpConv::NOTHING);
    Value* dblVal = irb.CreateSIToFP(op1, irb.getDoubleTy());
    auto* vec2d  = vecType(irb.getDoubleTy(), 2);
    Value* vDst  = irb.CreateBitCast(toI128(op0, irb), vec2d);
    Value* updated = irb.CreateInsertElement(vDst, dblVal, (uint64_t)0);
    storeOp(xi->operands[0], irb.CreateBitCast(updated, irb.getInt128Ty()),
            irb, eOpConv::NOTHING);
}

/**
 * x86 defines the answer for a float-to-signed-integer conversion whose input
 * does not fit -- a NaN, an infinity, or a magnitude past the destination's
 * range -- as the "integer indefinite" value: the destination's minimum
 * signed value. `cvttsd2si eax, xmm1` with xmm1 = +inf gives 0x80000000, and
 * so does a NaN, and so does 1.0e24.
 *
 * LLVM's fptosi calls all of that poison, which is a different thing and a
 * worse one: poison is not a value the decompiler can print, and it does not
 * stay where it is put once the optimiser sees it. Measured against the
 * hardware, the bare conversion disagreed on 652 of 4,805 rows -- every NaN,
 * every infinity and every out-of-range magnitude.
 *
 * So the input is range-checked, the conversion is fed a value that is always
 * in range, and the indefinite value is selected back in afterwards. The
 * bounds are -2^(N-1) and 2^(N-1): both are exactly representable in binary
 * floating point where 2^(N-1)-1 is not, and a STRICT upper comparison is the
 * correct boundary for a value that has already been rounded. Both
 * comparisons are ordered, so a NaN fails them and takes the indefinite path
 * without needing a test of its own.
 *
 * Works elementwise on a vector, which is what the packed forms need.
 */
llvm::Value*
Capstone2LlvmIrTranslatorX86_impl::generateFpToSiDefined(llvm::Value* v, llvm::Type* intTy, IRBuilder<>& irb)
{
	unsigned bits = intTy->getScalarType()->getIntegerBitWidth();
	double bound = std::ldexp(1.0, static_cast<int>(bits - 1));

	auto* lo = ConstantFP::get(v->getType(), -bound);
	auto* hi = ConstantFP::get(v->getType(), bound);
	auto* inRange = irb.CreateAnd(irb.CreateFCmpOGE(v, lo), irb.CreateFCmpOLT(v, hi));

	// Feed fptosi a value that is in range on every path, so the IR carries
	// no poison at all rather than poison that happens not to be selected.
	auto* safe = irb.CreateSelect(inRange, v, ConstantFP::get(v->getType(), 0.0));
	auto* conv = irb.CreateFPToSI(safe, intTy);
	auto* indefinite = ConstantInt::get(intTy, APInt::getSignedMinValue(bits));
	return irb.CreateSelect(inRange, conv, indefinite);
}

/**
 * CVTSD2SI — float64 lane 0 → int32/64, rounding; see translateCvtSs2Si.
 */
void Capstone2LlvmIrTranslatorX86_impl::translateCvtSd2Si(
        cs_insn* i, cs_x86* xi, IRBuilder<>& irb) {
    EXPECT_IS_BINARY(i, xi, irb);
    auto* src = loadOpBinaryOp1(xi, irb);
    auto* vec2d = vecType(irb.getDoubleTy(), 2);
    Value* v = irb.CreateBitCast(toI128(src, irb), vec2d);
    Value* lane0 = irb.CreateExtractElement(v, (uint64_t)0);
	lane0 = irb.CreateUnaryIntrinsic(Intrinsic::roundeven, lane0);
	unsigned destBits = xi->operands[0].size ? xi->operands[0].size * 8 : 32;
	Value* intVal = generateFpToSiDefined(lane0, irb.getIntNTy(destBits), irb);
	// ZEXT_TRUNC_OR_BITCAST: same fix as translateCvtSs2Si — GPR alloca
	// may be i64 while intVal is i32.
	storeOp(xi->operands[0], intVal, irb, eOpConv::ZEXT_TRUNC_OR_BITCAST);
}

/**
 * CVTDQ2PS — packed int32 → packed float32.
 */
void Capstone2LlvmIrTranslatorX86_impl::translateCvtDq2Ps(
        cs_insn* i, cs_x86* xi, IRBuilder<>& irb) {
    EXPECT_IS_BINARY(i, xi, irb);
    bool vex = (i->id == X86_INS_VCVTDQ2PS);
    unsigned destBits = vex ? (xi->operands[0].size * 8) : 128;
    if (vex && destBits != 128 && destBits != 256)
    {
        translatePseudoAsmGeneric(i, xi, irb);
        return;
    }
    unsigned loadBits = destBits;
    Value* src = vex ? loadVectorOp(xi->operands[1], irb, loadBits) : loadOpBinaryOp1(xi, irb);
    if (src == nullptr)
    {
        translatePseudoAsmGeneric(i, xi, irb);
        return;
    }
    unsigned n = destBits / 32;
    auto* vecI = vecType(irb.getInt32Ty(), n);
    auto* vecF = vecType(irb.getFloatTy(), n);
    Value* v = irb.CreateBitCast(irb.CreateZExtOrTrunc(vex ? src : toI128(src, irb), irb.getIntNTy(destBits)), vecI);
    Value* res = irb.CreateSIToFP(v, vecF);
    if (vex)
    {
        storeVectorOp(xi->operands[0], irb.CreateBitCast(res, irb.getIntNTy(destBits)), destBits, irb);
    }
    else
    {
        storeOp(xi->operands[0], irb.CreateBitCast(res, irb.getInt128Ty()), irb, eOpConv::NOTHING);
    }
}

/**
 * CVTPS2DQ — packed float32 → packed int32, rounding to nearest-even.
 * CVTTPS2DQ — the same, truncating toward zero.
 *
 * The extra T is the only difference between them and it was not being read:
 * this rounded nothing and CVTTPS2DQ was not in the table at all.
 */
void Capstone2LlvmIrTranslatorX86_impl::translateCvtPs2Dq(
        cs_insn* i, cs_x86* xi, IRBuilder<>& irb) {
    EXPECT_IS_BINARY(i, xi, irb);
    bool vex = (i->id == X86_INS_VCVTPS2DQ || i->id == X86_INS_VCVTTPS2DQ);
    bool trunc = (i->id == X86_INS_CVTTPS2DQ || i->id == X86_INS_VCVTTPS2DQ);
    unsigned destBits = vex ? (xi->operands[0].size * 8) : 128;
    if (vex && destBits != 128 && destBits != 256)
    {
        translatePseudoAsmGeneric(i, xi, irb);
        return;
    }
    Value* src = vex ? loadVectorOp(xi->operands[1], irb, destBits) : loadOpBinaryOp1(xi, irb);
    if (src == nullptr)
    {
        translatePseudoAsmGeneric(i, xi, irb);
        return;
    }
    unsigned n = destBits / 32;
    auto* vecF = vecType(irb.getFloatTy(), n);
    Value* rawBits = vex
		? irb.CreateZExtOrTrunc(src, irb.getIntNTy(destBits))
		: toI128(src, irb);
    Value* v = irb.CreateBitCast(rawBits, vecF);
    auto* packedTy = irb.getIntNTy(destBits);
    Value* packed = ConstantInt::get(packedTy, 0);
    for (unsigned lane = 0; lane < n; ++lane)
    {
        Value* e = irb.CreateExtractElement(v, (uint64_t)lane);
        if (!trunc)
        {
            e = irb.CreateUnaryIntrinsic(Intrinsic::roundeven, e);
        }
        Value* conv = generateFpToSiDefined(e, irb.getInt32Ty(), irb);
        Value* w = irb.CreateZExt(conv, packedTy);
        if (lane != 0)
        {
            w = irb.CreateShl(w, ConstantInt::get(packedTy, 32 * lane));
        }
        packed = irb.CreateOr(packed, w);
    }
    if (vex)
    {
        storeVectorOp(xi->operands[0], packed, destBits, irb);
    }
    else
    {
        storeOp(xi->operands[0], packed, irb, eOpConv::NOTHING);
    }
}

//===========================================================================
// SSE2 double precision, and the single-precision forms left out with it
//===========================================================================
//
// Everything below this line was `nullptr` in x86_init.cpp, and what that adds
// up to is that x86-64 -- the architecture every other one in this repository
// is measured against -- could not translate `double` arithmetic. The System V
// ABI passes and returns a double in an XMM register and gcc compiles `a + b`
// on doubles to ADDSD, so a floating-point program decompiled to a wall of
// `__asm_movsd` and `__asm_addsd` calls. Counted over the six floating-point
// programs in tests/algorithm_recovery/sources/generated, MOVSD alone is the
// second most frequent instruction in the whole text section, behind MOV.
//
// The machinery was already here: scalarFltBinOp and packedFltBinOp both take
// an `isDouble` flag and neither had a caller that passed true.

/// Does this operand name an XMM register?
static bool isXmmOp(const cs_x86_op& op)
{
	return op.type == X86_OP_REG && op.reg >= X86_REG_XMM0 && op.reg <= X86_REG_XMM31;
}

namespace {
/// Which of the four PS/SS/PD/SD shapes an instruction has.
struct FltForm
{
	bool isDouble;
	bool isScalar;
};
} // anonymous namespace

/**
 * The element type and lane count are in the mnemonic and nowhere else: PS is
 * four floats, PD two doubles, SS and SD the low lane of each. Spelled out per
 * id rather than derived from a suffix, because getting one of these wrong is
 * a silent miscompilation -- treating ADDSD as ADDPD adds a second lane that
 * the program never asked to be added.
 */
static bool fltForm(unsigned id, FltForm& out)
{
	switch (id)
	{
	case X86_INS_ADDPS:
	case X86_INS_SUBPS:
	case X86_INS_MULPS:
	case X86_INS_DIVPS:
	case X86_INS_MAXPS:
	case X86_INS_MINPS:
	case X86_INS_VMAXPS:
	case X86_INS_VMINPS:
	case X86_INS_SQRTPS: out = {false, false}; return true;
	case X86_INS_ADDSS:
	case X86_INS_SUBSS:
	case X86_INS_MULSS:
	case X86_INS_DIVSS:
	case X86_INS_MAXSS:
	case X86_INS_MINSS:
	case X86_INS_VADDSS:
	case X86_INS_VSUBSS:
	case X86_INS_VMULSS:
	case X86_INS_VDIVSS:
	case X86_INS_VMAXSS:
	case X86_INS_VMINSS:
	case X86_INS_VSQRTSS:
	case X86_INS_SQRTSS: out = {false, true}; return true;
	case X86_INS_ADDPD:
	case X86_INS_SUBPD:
	case X86_INS_MULPD:
	case X86_INS_DIVPD:
	case X86_INS_MAXPD:
	case X86_INS_MINPD:
	case X86_INS_VMAXPD:
	case X86_INS_VMINPD:
	case X86_INS_SQRTPD: out = {true, false}; return true;
	case X86_INS_ADDSD:
	case X86_INS_SUBSD:
	case X86_INS_MULSD:
	case X86_INS_DIVSD:
	case X86_INS_MAXSD:
	case X86_INS_MINSD:
	case X86_INS_VADDSD:
	case X86_INS_VSUBSD:
	case X86_INS_VMULSD:
	case X86_INS_VDIVSD:
	case X86_INS_VMAXSD:
	case X86_INS_VMINSD:
	case X86_INS_VSQRTSD:
	case X86_INS_SQRTSD: out = {true, true}; return true;
	default: return false;
	}
}

/**
 * ADDPS/ADDSS/ADDPD/ADDSD, SUBPS/SUBSS/SUBPD/SUBSD,
 * MULPS/MULSS/MULPD/MULSD, DIVPS/DIVSS/DIVPD/DIVSD.
 *
 * The scalar forms leave the upper lanes of the destination alone; that is
 * what scalarFltBinOp's insertelement is for, and it is the whole difference
 * between ADDSD and ADDPD.
 */
void Capstone2LlvmIrTranslatorX86_impl::translateSseFltArith(cs_insn* i, cs_x86* xi, IRBuilder<>& irb)
{
	FltForm f;
	if (!fltForm(i->id, f))
	{
		translatePseudoAsmGeneric(i, xi, irb);
		return;
	}

	unsigned opcode;
	switch (i->id)
	{
	case X86_INS_ADDPS:
	case X86_INS_ADDSS:
	case X86_INS_ADDPD:
	case X86_INS_ADDSD:
	case X86_INS_VADDSS:
	case X86_INS_VADDSD: opcode = Instruction::FAdd; break;
	case X86_INS_SUBPS:
	case X86_INS_SUBSS:
	case X86_INS_SUBPD:
	case X86_INS_SUBSD:
	case X86_INS_VSUBSS:
	case X86_INS_VSUBSD: opcode = Instruction::FSub; break;
	case X86_INS_MULPS:
	case X86_INS_MULSS:
	case X86_INS_MULPD:
	case X86_INS_MULSD:
	case X86_INS_VMULSS:
	case X86_INS_VMULSD: opcode = Instruction::FMul; break;
	case X86_INS_DIVPS:
	case X86_INS_DIVSS:
	case X86_INS_DIVPD:
	case X86_INS_DIVSD:
	case X86_INS_VDIVSS:
	case X86_INS_VDIVSD: opcode = Instruction::FDiv; break;
	default: translatePseudoAsmGeneric(i, xi, irb); return;
	}

	// AVX scalar: dest[low] = src1[low] op src2[low], dest[upper] = src1[upper].
	if (i->id == X86_INS_VADDSS || i->id == X86_INS_VADDSD || i->id == X86_INS_VSUBSS
		|| i->id == X86_INS_VSUBSD || i->id == X86_INS_VMULSS || i->id == X86_INS_VMULSD
		|| i->id == X86_INS_VDIVSS || i->id == X86_INS_VDIVSD)
	{
		if (xi->op_count != 3)
		{
			translatePseudoAsmGeneric(i, xi, irb);
			return;
		}
		Value* a = loadOp(xi->operands[1], irb);
		Value* b = loadOp(xi->operands[2], irb);
		storeVectorOp(xi->operands[0], scalarFltBinOp(a, b, f.isDouble, opcode, irb), 128, irb);
		return;
	}

	EXPECT_IS_BINARY(i, xi, irb);
	std::tie(op0, op1) = loadOpBinary(xi, irb, eOpConv::NOTHING);
	Value* res = f.isScalar ? scalarFltBinOp(op0, op1, f.isDouble, opcode, irb)
							: packedFltBinOp(op0, op1, f.isDouble, opcode, irb);
	storeOp(xi->operands[0], res, irb, eOpConv::NOTHING);
}

/**
 * MAXPS/MAXSS/MAXPD/MAXSD, MINPS/MINSS/MINPD/MINSD.
 *
 * Not llvm.maxnum. x86's MAXSD is defined as `dst > src ? dst : src`, which
 * returns the SECOND operand whenever either is a NaN and whenever both are
 * zeros of opposite sign -- the opposite of what llvm.maxnum does with a NaN.
 * The select spells the architecture's definition directly.
 */
void Capstone2LlvmIrTranslatorX86_impl::translateSseFltMinMax(cs_insn* i, cs_x86* xi, IRBuilder<>& irb)
{
	FltForm f;
	if (!fltForm(i->id, f))
	{
		translatePseudoAsmGeneric(i, xi, irb);
		return;
	}
	bool isMax = i->id == X86_INS_MAXPS || i->id == X86_INS_MAXSS || i->id == X86_INS_MAXPD || i->id == X86_INS_MAXSD
		|| i->id == X86_INS_VMAXPS || i->id == X86_INS_VMAXSS || i->id == X86_INS_VMAXPD || i->id == X86_INS_VMAXSD;

	Type* elemTy = f.isDouble ? irb.getDoubleTy() : irb.getFloatTy();
	unsigned laneBits = f.isDouble ? 64 : 32;

	auto minmaxLane = [&](Value* a, Value* b) {
		Value* c = isMax ? irb.CreateFCmpOGT(a, b) : irb.CreateFCmpOLT(a, b);
		return irb.CreateSelect(c, a, b);
	};

	// AVX packed: dest = min/max(src1, src2) over every lane.
	if (i->id == X86_INS_VMAXPS || i->id == X86_INS_VMINPS || i->id == X86_INS_VMAXPD || i->id == X86_INS_VMINPD)
	{
		if (xi->op_count != 3)
		{
			translatePseudoAsmGeneric(i, xi, irb);
			return;
		}
		unsigned destBits = xi->operands[0].size * 8;
		if (destBits != 128 && destBits != 256)
		{
			translatePseudoAsmGeneric(i, xi, irb);
			return;
		}
		unsigned bits = destBits;
		Value* a = loadVectorOp(xi->operands[1], irb, bits);
		bits = destBits;
		Value* b = loadVectorOp(xi->operands[2], irb, bits);
		if (a == nullptr || b == nullptr)
		{
			translatePseudoAsmGeneric(i, xi, irb);
			return;
		}
		unsigned n = destBits / laneBits;
		auto* vecTy = FixedVectorType::get(elemTy, n);
		Value* vDst = irb.CreateBitCast(irb.CreateZExtOrTrunc(a, irb.getIntNTy(destBits)), vecTy);
		Value* vSrc = irb.CreateBitCast(irb.CreateZExtOrTrunc(b, irb.getIntNTy(destBits)), vecTy);
		Value* res = UndefValue::get(vecTy);
		for (unsigned lane = 0; lane < n; ++lane)
		{
			res = irb.CreateInsertElement(
				res,
				minmaxLane(irb.CreateExtractElement(vDst, (uint64_t)lane), irb.CreateExtractElement(vSrc, (uint64_t)lane)),
				(uint64_t)lane);
		}
		storeVectorOp(xi->operands[0], irb.CreateBitCast(res, irb.getIntNTy(destBits)), destBits, irb);
		return;
	}

	// AVX scalar: dest[low] = min/max(src1[low], src2[low]), dest[upper] = src1.
	if (i->id == X86_INS_VMAXSS || i->id == X86_INS_VMINSS || i->id == X86_INS_VMAXSD || i->id == X86_INS_VMINSD)
	{
		if (xi->op_count != 3)
		{
			translatePseudoAsmGeneric(i, xi, irb);
			return;
		}
		unsigned n = f.isDouble ? 2 : 4;
		auto* vecTy = FixedVectorType::get(elemTy, n);
		Value* vDst = irb.CreateBitCast(toI128(loadOp(xi->operands[1], irb), irb), vecTy);
		Value* vSrc = irb.CreateBitCast(toI128(loadOp(xi->operands[2], irb), irb), vecTy);
		Value* r = minmaxLane(irb.CreateExtractElement(vDst, (uint64_t)0), irb.CreateExtractElement(vSrc, (uint64_t)0));
		storeVectorOp(
			xi->operands[0],
			irb.CreateBitCast(irb.CreateInsertElement(vDst, r, (uint64_t)0), irb.getInt128Ty()),
			128,
			irb);
		return;
	}

	EXPECT_IS_BINARY(i, xi, irb);
	std::tie(op0, op1) = loadOpBinary(xi, irb, eOpConv::NOTHING);

	unsigned n = f.isDouble ? 2 : 4;
	auto* vecTy = FixedVectorType::get(elemTy, n);
	Value* vDst = irb.CreateBitCast(toI128(op0, irb), vecTy);
	Value* vSrc = irb.CreateBitCast(toI128(op1, irb), vecTy);

	if (f.isScalar)
	{
		Value* a = irb.CreateExtractElement(vDst, (uint64_t)0);
		Value* b = irb.CreateExtractElement(vSrc, (uint64_t)0);
		Value* c = isMax ? irb.CreateFCmpOGT(a, b) : irb.CreateFCmpOLT(a, b);
		Value* r = irb.CreateSelect(c, a, b);
		storeOp(
			xi->operands[0],
			irb.CreateBitCast(irb.CreateInsertElement(vDst, r, (uint64_t)0), irb.getInt128Ty()),
			irb,
			eOpConv::NOTHING);
		return;
	}

	Value* res = UndefValue::get(vecTy);
	for (unsigned lane = 0; lane < n; ++lane)
	{
		Value* a = irb.CreateExtractElement(vDst, (uint64_t)lane);
		Value* b = irb.CreateExtractElement(vSrc, (uint64_t)lane);
		Value* c = isMax ? irb.CreateFCmpOGT(a, b) : irb.CreateFCmpOLT(a, b);
		res = irb.CreateInsertElement(res, irb.CreateSelect(c, a, b), (uint64_t)lane);
	}
	storeOp(xi->operands[0], irb.CreateBitCast(res, irb.getInt128Ty()), irb, eOpConv::NOTHING);
}

/**
 * SQRTPS/SQRTSS/SQRTPD/SQRTSD.
 *
 * The scalar forms take the square root of the SOURCE's low lane and keep the
 * DESTINATION's upper lanes, which is not the shape any of the arithmetic
 * above has and is easy to write as sqrt-in-place by mistake.
 */
void Capstone2LlvmIrTranslatorX86_impl::translateSseSqrt(cs_insn* i, cs_x86* xi, IRBuilder<>& irb)
{
	FltForm f;
	if (!fltForm(i->id, f))
	{
		translatePseudoAsmGeneric(i, xi, irb);
		return;
	}

	Type* elemTy = f.isDouble ? irb.getDoubleTy() : irb.getFloatTy();
	unsigned n = f.isDouble ? 2 : 4;
	auto* vecTy = FixedVectorType::get(elemTy, n);
	auto sqrtOf = [&](Value* v) { return irb.CreateUnaryIntrinsic(Intrinsic::sqrt, v); };

	// AVX scalar: dest[low] = sqrt(src2[low]), dest[upper] = src1[upper].
	if (i->id == X86_INS_VSQRTSS || i->id == X86_INS_VSQRTSD)
	{
		if (xi->op_count != 3)
		{
			translatePseudoAsmGeneric(i, xi, irb);
			return;
		}
		Value* vDst = irb.CreateBitCast(toI128(loadOp(xi->operands[1], irb), irb), vecTy);
		Value* vSrc = irb.CreateBitCast(toI128(loadOp(xi->operands[2], irb), irb), vecTy);
		Value* r = sqrtOf(irb.CreateExtractElement(vSrc, (uint64_t)0));
		storeVectorOp(
			xi->operands[0],
			irb.CreateBitCast(irb.CreateInsertElement(vDst, r, (uint64_t)0), irb.getInt128Ty()),
			128,
			irb);
		return;
	}

	EXPECT_IS_BINARY(i, xi, irb);
	std::tie(op0, op1) = loadOpBinary(xi, irb, eOpConv::NOTHING);
	Value* vSrc = irb.CreateBitCast(toI128(op1, irb), vecTy);

	if (f.isScalar)
	{
		Value* vDst = irb.CreateBitCast(toI128(op0, irb), vecTy);
		Value* r = sqrtOf(irb.CreateExtractElement(vSrc, (uint64_t)0));
		storeOp(
			xi->operands[0],
			irb.CreateBitCast(irb.CreateInsertElement(vDst, r, (uint64_t)0), irb.getInt128Ty()),
			irb,
			eOpConv::NOTHING);
		return;
	}

	Value* res = UndefValue::get(vecTy);
	for (unsigned lane = 0; lane < n; ++lane)
	{
		res = irb.CreateInsertElement(res, sqrtOf(irb.CreateExtractElement(vSrc, (uint64_t)lane)), (uint64_t)lane);
	}
	storeOp(xi->operands[0], irb.CreateBitCast(res, irb.getInt128Ty()), irb, eOpConv::NOTHING);
}

/**
 * ROUNDSS/ROUNDSD/ROUNDPS/ROUNDPD, and the 3-operand VEX twins
 * VROUNDPS/VROUNDPD.
 *
 * SDM imm8 bits 0-1:
 *   00 nearbyint (nearest, ties even)
 *   01 floor
 *   10 ceil
 *   11 trunc
 * bit 2 set means "use MXCSR" — approximated as nearbyint, the same
 * stand-in ARM64 FRINTN uses for current-mode rounding.
 *
 * Scalar forms round the SOURCE's low lane and keep the upper lanes of
 * another operand (SQRTSS's shape). Packed forms round every lane of the
 * source. VROUNDSS/VROUNDSD are 4-operand (dst, src1-upper, src2, imm);
 * the upper lanes come from src1, not from dest.
 */
void Capstone2LlvmIrTranslatorX86_impl::translateSseRound(cs_insn* i, cs_x86* xi, IRBuilder<>& irb)
{
	bool vexScalar = i->id == X86_INS_VROUNDSS || i->id == X86_INS_VROUNDSD
		|| i->id == X86_INS_VRNDSCALESS || i->id == X86_INS_VRNDSCALESD;
	unsigned immOp = vexScalar ? 3u : 2u;
	unsigned srcOp = vexScalar ? 2u : 1u;
	unsigned keepOp = vexScalar ? 1u : 0u;
	if (vexScalar)
	{
		if (xi->op_count != 4 || xi->operands[immOp].type != X86_OP_IMM)
		{
			translatePseudoAsmGeneric(i, xi, irb);
			return;
		}
	}
	else
	{
		EXPECT_IS_TERNARY(i, xi, irb);
		if (xi->operands[immOp].type != X86_OP_IMM || xi->operands[0].size != 16)
		{
			translatePseudoAsmGeneric(i, xi, irb);
			return;
		}
	}

	unsigned imm = static_cast<unsigned>(xi->operands[immOp].imm) & 0xff;
	Intrinsic::ID iid;
	if (imm & 4)
	{
		iid = Intrinsic::nearbyint;
	}
	else
	{
		switch (imm & 3)
		{
		case 0: iid = Intrinsic::nearbyint; break;
		case 1: iid = Intrinsic::floor; break;
		case 2: iid = Intrinsic::ceil; break;
		default: iid = Intrinsic::trunc; break;
		}
	}

	bool isDouble = i->id == X86_INS_ROUNDSD || i->id == X86_INS_ROUNDPD
		|| i->id == X86_INS_VROUNDPD || i->id == X86_INS_VROUNDSD
		|| i->id == X86_INS_VRNDSCALEPD || i->id == X86_INS_VRNDSCALESD;
	bool isScalar = i->id == X86_INS_ROUNDSS || i->id == X86_INS_ROUNDSD
		|| i->id == X86_INS_VROUNDSS || i->id == X86_INS_VROUNDSD
		|| i->id == X86_INS_VRNDSCALESS || i->id == X86_INS_VRNDSCALESD;

	Value* keep = toI128(loadOp(xi->operands[keepOp], irb), irb);
	Value* src = toI128(loadOp(xi->operands[srcOp], irb), irb);

	Type* elemTy = isDouble ? irb.getDoubleTy() : irb.getFloatTy();
	unsigned n = isDouble ? 2 : 4;
	auto* vecTy = FixedVectorType::get(elemTy, n);
	Value* vSrc = irb.CreateBitCast(src, vecTy);
	auto roundOf = [&](Value* v) { return irb.CreateUnaryIntrinsic(iid, v); };

	Value* asInt = nullptr;
	if (isScalar)
	{
		Value* vDst = irb.CreateBitCast(keep, vecTy);
		Value* r = roundOf(irb.CreateExtractElement(vSrc, (uint64_t)0));
		asInt = irb.CreateBitCast(irb.CreateInsertElement(vDst, r, (uint64_t)0), irb.getInt128Ty());
	}
	else
	{
		asInt = irb.CreateBitCast(roundOf(vSrc), irb.getInt128Ty());
	}

	if (vexScalar)
	{
		storeVectorOp(xi->operands[0], asInt, 128, irb);
	}
	else
	{
		storeOp(xi->operands[0], asInt, irb, eOpConv::NOTHING);
	}
}

/**
 * UCOMISS, UCOMISD, COMISS, COMISD — compare the low lanes and write EFLAGS.
 *
 * The architecture's table is
 *
 *     unordered  ZF=1 PF=1 CF=1
 *     less       ZF=0 PF=0 CF=1
 *     equal      ZF=1 PF=0 CF=0
 *     greater    ZF=0 PF=0 CF=0
 *
 * which is three unordered-inclusive comparisons and no branching at all:
 * ZF is `unordered or equal`, CF is `unordered or less`, PF is `unordered`.
 * OF, AF and SF are cleared.
 *
 * COMIS* differs from UCOMIS* only in which NaNs raise an exception, and this
 * lifter has no floating-point exception state to raise into, so the two are
 * the same translation.
 */
void Capstone2LlvmIrTranslatorX86_impl::translateSseComi(cs_insn* i, cs_x86* xi, IRBuilder<>& irb)
{
	EXPECT_IS_BINARY(i, xi, irb);

	bool isDouble =
		i->id == X86_INS_UCOMISD || i->id == X86_INS_COMISD || i->id == X86_INS_VUCOMISD || i->id == X86_INS_VCOMISD;
	Type* elemTy = isDouble ? irb.getDoubleTy() : irb.getFloatTy();
	unsigned n = isDouble ? 2 : 4;
	auto* vecTy = FixedVectorType::get(elemTy, n);

	std::tie(op0, op1) = loadOpBinary(xi, irb, eOpConv::NOTHING);
	Value* a = irb.CreateExtractElement(irb.CreateBitCast(toI128(op0, irb), vecTy), (uint64_t)0);
	Value* b = irb.CreateExtractElement(irb.CreateBitCast(toI128(op1, irb), vecTy), (uint64_t)0);

	storeRegister(X86_REG_ZF, irb.CreateFCmpUEQ(a, b), irb);
	storeRegister(X86_REG_PF, irb.CreateFCmpUNO(a, b), irb);
	storeRegister(X86_REG_CF, irb.CreateFCmpULT(a, b), irb);
	storeRegister(X86_REG_OF, irb.getFalse(), irb);
	storeRegister(X86_REG_AF, irb.getFalse(), irb);
	storeRegister(X86_REG_SF, irb.getFalse(), irb);
}

/**
 * ANDPS/ANDPD, ANDNPS/ANDNPD, ORPS/ORPD, XORPS/XORPD.
 *
 * Bitwise on the whole 128 bits; the PS/PD distinction carries no information
 * at all here. They are in every floating-point binary because they are how a
 * compiler writes negation and absolute value: `xorpd xmm0, [sign mask]` and
 * `andpd xmm0, [magnitude mask]`.
 *
 * ANDN is `(~dst) & src`, complementing the DESTINATION -- the reverse of the
 * argument order the mnemonic suggests.
 */
void Capstone2LlvmIrTranslatorX86_impl::translateSseFltLogic(cs_insn* i, cs_x86* xi, IRBuilder<>& irb)
{
	auto logicOf = [&](unsigned id, Value* a, Value* b, IRBuilder<>& builder) -> Value* {
		switch (id)
		{
		case X86_INS_ANDPS:
		case X86_INS_ANDPD:
		case X86_INS_VANDPS:
		case X86_INS_VANDPD: return builder.CreateAnd(a, b);
		case X86_INS_ANDNPS:
		case X86_INS_ANDNPD:
		case X86_INS_VANDNPS:
		case X86_INS_VANDNPD: return builder.CreateAnd(builder.CreateNot(a), b);
		case X86_INS_ORPS:
		case X86_INS_ORPD:
		case X86_INS_VORPS:
		case X86_INS_VORPD: return builder.CreateOr(a, b);
		case X86_INS_XORPS:
		case X86_INS_XORPD:
		case X86_INS_VXORPS:
		case X86_INS_VXORPD: return builder.CreateXor(a, b);
		default: return nullptr;
		}
	};

	if (i->id == X86_INS_VANDPS || i->id == X86_INS_VANDPD || i->id == X86_INS_VANDNPS || i->id == X86_INS_VANDNPD
		|| i->id == X86_INS_VORPS || i->id == X86_INS_VORPD || i->id == X86_INS_VXORPS || i->id == X86_INS_VXORPD)
	{
		if (xi->op_count != 3)
		{
			translatePseudoAsmGeneric(i, xi, irb);
			return;
		}
		unsigned destBits = xi->operands[0].size * 8;
		if (destBits != 128 && destBits != 256)
		{
			translatePseudoAsmGeneric(i, xi, irb);
			return;
		}
		unsigned bits = destBits;
		Value* a = loadVectorOp(xi->operands[1], irb, bits);
		bits = destBits;
		Value* b = loadVectorOp(xi->operands[2], irb, bits);
		if (a == nullptr || b == nullptr)
		{
			translatePseudoAsmGeneric(i, xi, irb);
			return;
		}
		auto* wide = irb.getIntNTy(destBits);
		Value* res = logicOf(i->id, irb.CreateZExtOrTrunc(a, wide), irb.CreateZExtOrTrunc(b, wide), irb);
		if (res == nullptr)
		{
			translatePseudoAsmGeneric(i, xi, irb);
			return;
		}
		storeVectorOp(xi->operands[0], res, destBits, irb);
		return;
	}

	EXPECT_IS_BINARY(i, xi, irb);
	std::tie(op0, op1) = loadOpBinary(xi, irb, eOpConv::NOTHING);
	Value* a = toI128(op0, irb);
	Value* b = toI128(op1, irb);
	Value* res = logicOf(i->id, a, b, irb);
	if (res == nullptr)
	{
		translatePseudoAsmGeneric(i, xi, irb);
		return;
	}
	storeOp(xi->operands[0], res, irb, eOpConv::NOTHING);
}

/**
 * MOVSS, and the SSE form of MOVSD.
 *
 * Capstone gives `movsd xmm0, qword ptr [rdi]` and the string instruction
 * `movsd` the same id, X86_INS_MOVSD, and this table entry used to point at
 * translateMoveString, which rejects anything whose operands are not both
 * memory -- so the most common instruction in x86-64 floating-point code came
 * out as a pseudo-asm call. The two are told apart here by whether an operand
 * names an XMM register, which is the only thing that distinguishes them.
 *
 * Three forms, and the difference between the first two is the whole reason
 * this cannot be translateSseMovWhole:
 *
 *     movsd xmm1, xmm2   low lane from xmm2, upper lanes of xmm1 PRESERVED
 *     movsd xmm1, m64    low lane from memory, upper lanes ZEROED
 *     movsd m64, xmm1    low lane to memory
 */
void Capstone2LlvmIrTranslatorX86_impl::translateSseMovScalar(cs_insn* i, cs_x86* xi, IRBuilder<>& irb)
{
	if (i->id == X86_INS_MOVSD && xi->op_count == 2 && !isXmmOp(xi->operands[0]) && !isXmmOp(xi->operands[1]))
	{
		translateMoveString(i, xi, irb);
		return;
	}

	unsigned bits = (i->id == X86_INS_MOVSD || i->id == X86_INS_VMOVSD
		|| i->id == X86_INS_MOVNTSD) ? 64 : 32;
	auto* laneTy = irb.getIntNTy(bits);
	bool vex = (i->id == X86_INS_VMOVSD || i->id == X86_INS_VMOVSS);

	// AVX 3-operand register form: dest[low] = src2[low], dest[upper] = src1.
	if (vex && xi->op_count == 3)
	{
		Value* keepSrc = toI128(loadOp(xi->operands[1], irb), irb);
		Value* laneSrc = toI128(loadOp(xi->operands[2], irb), irb);
		Value* lane = irb.CreateZExt(irb.CreateTrunc(laneSrc, laneTy), irb.getInt128Ty());
		Value* keep = irb.CreateAnd(keepSrc, ConstantInt::get(irb.getInt128Ty(), APInt::getHighBitsSet(128, 128 - bits)));
		storeVectorOp(xi->operands[0], irb.CreateOr(keep, lane), 128, irb);
		return;
	}

	EXPECT_IS_BINARY(i, xi, irb);

	if (xi->operands[0].type == X86_OP_MEM)
	{
		Value* src = loadOp(xi->operands[1], irb);
		storeOp(xi->operands[0], irb.CreateTrunc(toI128(src, irb), laneTy), irb, eOpConv::NOTHING);
		return;
	}

	Value* src = loadOp(xi->operands[1], irb);
	Value* lane = irb.CreateZExt(irb.CreateTrunc(toI128(src, irb), laneTy), irb.getInt128Ty());

	if (xi->operands[1].type == X86_OP_MEM)
	{
		if (vex)
		{
			storeVectorOp(xi->operands[0], lane, 128, irb);
		}
		else
		{
			storeOp(xi->operands[0], lane, irb, eOpConv::NOTHING);
		}
		return;
	}

	Value* dst = toI128(loadOp(xi->operands[0], irb), irb);
	Value* keep = irb.CreateAnd(dst, ConstantInt::get(irb.getInt128Ty(), APInt::getHighBitsSet(128, 128 - bits)));
	storeOp(xi->operands[0], irb.CreateOr(keep, lane), irb, eOpConv::NOTHING);
}

/**
 * MOVLPS/MOVLPD — move the low 64 bits, keeping the high 64.
 * MOVHPS/MOVHPD — move the high 64 bits, keeping the low 64.
 *
 * Both halves matter in both directions, so neither is a whole-register move
 * and neither is translateSseMovScalar's zeroing memory form.
 */
void Capstone2LlvmIrTranslatorX86_impl::translateSseMovHalf(cs_insn* i, cs_x86* xi, IRBuilder<>& irb)
{
	bool high = i->id == X86_INS_MOVHPS || i->id == X86_INS_MOVHPD || i->id == X86_INS_VMOVHPS
		|| i->id == X86_INS_VMOVHPD;
	auto* i128 = irb.getInt128Ty();
	auto* i64t = irb.getInt64Ty();
	bool vex = (i->id == X86_INS_VMOVLPS || i->id == X86_INS_VMOVLPD || i->id == X86_INS_VMOVHPS
		|| i->id == X86_INS_VMOVHPD);

	// AVX xmm dest is dest, src1-keep, src2-moved (usually m64).
	if (vex && xi->op_count == 3)
	{
		Value* dst = toI128(loadOp(xi->operands[1], irb), irb);
		Value* src = irb.CreateZExt(irb.CreateTrunc(toI128(loadOp(xi->operands[2], irb), irb), i64t), i128);
		if (high)
		{
			src = irb.CreateShl(src, ConstantInt::get(i128, 64));
		}
		Value* keep = irb.CreateAnd(
			dst, ConstantInt::get(i128, high ? APInt::getLowBitsSet(128, 64) : APInt::getHighBitsSet(128, 64)));
		storeVectorOp(xi->operands[0], irb.CreateOr(keep, src), 128, irb);
		return;
	}

	EXPECT_IS_BINARY(i, xi, irb);

	if (xi->operands[0].type == X86_OP_MEM)
	{
		Value* src = toI128(loadOp(xi->operands[1], irb), irb);
		if (high)
		{
			src = irb.CreateLShr(src, ConstantInt::get(i128, 64));
		}
		storeOp(xi->operands[0], irb.CreateTrunc(src, i64t), irb, eOpConv::NOTHING);
		return;
	}

	Value* dst = toI128(loadOp(xi->operands[0], irb), irb);
	Value* src = irb.CreateZExt(irb.CreateTrunc(toI128(loadOp(xi->operands[1], irb), irb), i64t), i128);
	if (high)
	{
		src = irb.CreateShl(src, ConstantInt::get(i128, 64));
	}
	Value* keep = irb.CreateAnd(
		dst, ConstantInt::get(i128, high ? APInt::getLowBitsSet(128, 64) : APInt::getHighBitsSet(128, 64)));
	storeOp(xi->operands[0], irb.CreateOr(keep, src), irb, eOpConv::NOTHING);
}

/**
 * CVTSS2SD, CVTSD2SS — widen or narrow the low lane, keeping the rest.
 * CVTPS2PD — the low two floats become two doubles.
 * CVTPD2PS — two doubles become the low two floats, the upper two zeroed.
 * CVTDQ2PD — the low two int32s become two doubles.
 */
void Capstone2LlvmIrTranslatorX86_impl::translateSseCvtFlt(cs_insn* i, cs_x86* xi, IRBuilder<>& irb)
{
	auto* i128 = irb.getInt128Ty();
	auto* vec4f = vecType(irb.getFloatTy(), 4);
	auto* vec2d = vecType(irb.getDoubleTy(), 2);
	auto* vec4i = vecType(irb.getInt32Ty(), 4);

	// AVX 3-operand scalar convert: dest[low] = cvt(src2), dest[upper] = src1.
	if (i->id == X86_INS_VCVTSS2SD || i->id == X86_INS_VCVTSD2SS)
	{
		EXPECT_IS_TERNARY(i, xi, irb);
		Value* preserve = toI128(loadOp(xi->operands[1], irb), irb);
		Value* src = toI128(loadOp(xi->operands[2], irb), irb);
		if (i->id == X86_INS_VCVTSS2SD)
		{
			Value* lane = irb.CreateExtractElement(irb.CreateBitCast(src, vec4f), (uint64_t)0);
			Value* wide = irb.CreateFPExt(lane, irb.getDoubleTy());
			Value* dst = irb.CreateBitCast(preserve, vec2d);
			storeVectorOp(
				xi->operands[0],
				irb.CreateBitCast(irb.CreateInsertElement(dst, wide, (uint64_t)0), i128),
				128,
				irb);
			return;
		}
		Value* lane = irb.CreateExtractElement(irb.CreateBitCast(src, vec2d), (uint64_t)0);
		Value* narrow = irb.CreateFPTrunc(lane, irb.getFloatTy());
		Value* dst = irb.CreateBitCast(preserve, vec4f);
		storeVectorOp(
			xi->operands[0],
			irb.CreateBitCast(irb.CreateInsertElement(dst, narrow, (uint64_t)0), i128),
			128,
			irb);
		return;
	}

	if (i->id == X86_INS_VCVTPS2PD || i->id == X86_INS_VCVTPD2PS || i->id == X86_INS_VCVTDQ2PD)
	{
		EXPECT_IS_BINARY(i, xi, irb);
		unsigned destBits = xi->operands[0].size * 8;
		unsigned srcBits = xi->operands[1].size * 8;
		if ((destBits != 128 && destBits != 256) || srcBits == 0)
		{
			translatePseudoAsmGeneric(i, xi, irb);
			return;
		}
		unsigned loadBits = srcBits >= 128 ? srcBits : 128;
		Value* raw = loadVectorOp(xi->operands[1], irb, loadBits);
		if (raw == nullptr)
		{
			raw = toI128(loadOp(xi->operands[1], irb), irb);
			srcBits = 128;
		}
		if (i->id == X86_INS_VCVTPS2PD)
		{
			unsigned n = destBits / 64;
			unsigned srcElems = srcBits / 32;
			if (srcElems < n)
			{
				srcElems = n < 4 ? 4 : n;
			}
			auto* srcTy = vecType(irb.getFloatTy(), srcElems);
			auto* dstTy = vecType(irb.getDoubleTy(), n);
			Value* v = irb.CreateBitCast(irb.CreateZExtOrTrunc(raw, irb.getIntNTy(srcElems * 32)), srcTy);
			Value* res = UndefValue::get(dstTy);
			for (unsigned lane = 0; lane < n; ++lane)
			{
				res = irb.CreateInsertElement(
					res,
					irb.CreateFPExt(irb.CreateExtractElement(v, (uint64_t)lane), irb.getDoubleTy()),
					(uint64_t)lane);
			}
			storeVectorOp(xi->operands[0], irb.CreateBitCast(res, irb.getIntNTy(destBits)), destBits, irb);
			return;
		}
		if (i->id == X86_INS_VCVTDQ2PD)
		{
			unsigned n = destBits / 64;
			unsigned srcElems = srcBits / 32;
			if (srcElems < n)
			{
				srcElems = n < 4 ? 4 : n;
			}
			auto* srcTy = vecType(irb.getInt32Ty(), srcElems);
			auto* dstTy = vecType(irb.getDoubleTy(), n);
			Value* v = irb.CreateBitCast(irb.CreateZExtOrTrunc(raw, irb.getIntNTy(srcElems * 32)), srcTy);
			Value* res = UndefValue::get(dstTy);
			for (unsigned lane = 0; lane < n; ++lane)
			{
				res = irb.CreateInsertElement(
					res,
					irb.CreateSIToFP(irb.CreateExtractElement(v, (uint64_t)lane), irb.getDoubleTy()),
					(uint64_t)lane);
			}
			storeVectorOp(xi->operands[0], irb.CreateBitCast(res, irb.getIntNTy(destBits)), destBits, irb);
			return;
		}
		unsigned n = srcBits / 64;
		if (n == 0)
		{
			n = 2;
			srcBits = 128;
		}
		auto* srcTy = vecType(irb.getDoubleTy(), n);
		auto* dstTy = vecType(irb.getFloatTy(), destBits / 32);
		Value* v = irb.CreateBitCast(irb.CreateZExtOrTrunc(raw, irb.getIntNTy(srcBits)), srcTy);
		Value* res = ConstantAggregateZero::get(dstTy);
		for (unsigned lane = 0; lane < n && lane < destBits / 32; ++lane)
		{
			res = irb.CreateInsertElement(
				res,
				irb.CreateFPTrunc(irb.CreateExtractElement(v, (uint64_t)lane), irb.getFloatTy()),
				(uint64_t)lane);
		}
		storeVectorOp(xi->operands[0], irb.CreateBitCast(res, irb.getIntNTy(destBits)), destBits, irb);
		return;
	}

	EXPECT_IS_BINARY(i, xi, irb);

	std::tie(op0, op1) = loadOpBinary(xi, irb, eOpConv::NOTHING);
	Value* src = toI128(op1, irb);

	switch (i->id)
	{
	case X86_INS_CVTSS2SD: {
		Value* lane = irb.CreateExtractElement(irb.CreateBitCast(src, vec4f), (uint64_t)0);
		Value* wide = irb.CreateFPExt(lane, irb.getDoubleTy());
		Value* dst = irb.CreateBitCast(toI128(op0, irb), vec2d);
		storeOp(
			xi->operands[0],
			irb.CreateBitCast(irb.CreateInsertElement(dst, wide, (uint64_t)0), i128),
			irb,
			eOpConv::NOTHING);
		return;
	}
	case X86_INS_CVTSD2SS: {
		Value* lane = irb.CreateExtractElement(irb.CreateBitCast(src, vec2d), (uint64_t)0);
		Value* narrow = irb.CreateFPTrunc(lane, irb.getFloatTy());
		Value* dst = irb.CreateBitCast(toI128(op0, irb), vec4f);
		storeOp(
			xi->operands[0],
			irb.CreateBitCast(irb.CreateInsertElement(dst, narrow, (uint64_t)0), i128),
			irb,
			eOpConv::NOTHING);
		return;
	}
	case X86_INS_CVTPS2PD: {
		Value* v = irb.CreateBitCast(src, vec4f);
		Value* res = UndefValue::get(vec2d);
		for (unsigned lane = 0; lane < 2; ++lane)
		{
			res = irb.CreateInsertElement(
				res, irb.CreateFPExt(irb.CreateExtractElement(v, (uint64_t)lane), irb.getDoubleTy()), (uint64_t)lane);
		}
		storeOp(xi->operands[0], irb.CreateBitCast(res, i128), irb, eOpConv::NOTHING);
		return;
	}
	case X86_INS_CVTPD2PS: {
		Value* v = irb.CreateBitCast(src, vec2d);
		Value* res = ConstantAggregateZero::get(vec4f);
		for (unsigned lane = 0; lane < 2; ++lane)
		{
			res = irb.CreateInsertElement(
				res, irb.CreateFPTrunc(irb.CreateExtractElement(v, (uint64_t)lane), irb.getFloatTy()), (uint64_t)lane);
		}
		storeOp(xi->operands[0], irb.CreateBitCast(res, i128), irb, eOpConv::NOTHING);
		return;
	}
	case X86_INS_CVTDQ2PD: {
		Value* v = irb.CreateBitCast(src, vec4i);
		Value* res = UndefValue::get(vec2d);
		for (unsigned lane = 0; lane < 2; ++lane)
		{
			res = irb.CreateInsertElement(
				res, irb.CreateSIToFP(irb.CreateExtractElement(v, (uint64_t)lane), irb.getDoubleTy()), (uint64_t)lane);
		}
		storeOp(xi->operands[0], irb.CreateBitCast(res, i128), irb, eOpConv::NOTHING);
		return;
	}
	default: translatePseudoAsmGeneric(i, xi, irb); return;
	}
}

/**
 * CVTTSS2SI, CVTTSD2SI — low lane to integer, truncating toward zero.
 *
 * The rounding one is CVTSS2SI/CVTSD2SI above; this is the pair a C cast from
 * double to int compiles to, and gcc emits CVTTSD2SI nine times in the six
 * floating-point corpus programs.
 *
 * The destination width comes from the operand, not from the address size:
 * `cvttsd2si eax, xmm0` is a 32-bit conversion in a 64-bit program.
 */
void Capstone2LlvmIrTranslatorX86_impl::translateCvtTt2Si(cs_insn* i, cs_x86* xi, IRBuilder<>& irb)
{
	EXPECT_IS_BINARY(i, xi, irb);

	bool isDouble = i->id == X86_INS_CVTTSD2SI || i->id == X86_INS_VCVTTSD2SI;
	auto* vecTy = isDouble ? vecType(irb.getDoubleTy(), 2) : vecType(irb.getFloatTy(), 4);
	Value* src = toI128(loadOp(xi->operands[1], irb), irb);
	Value* lane = irb.CreateExtractElement(irb.CreateBitCast(src, vecTy), (uint64_t)0);

	unsigned destBits = xi->operands[0].size ? xi->operands[0].size * 8 : 32;
	storeOp(
		xi->operands[0],
		generateFpToSiDefined(lane, irb.getIntNTy(destBits), irb),
		irb,
		eOpConv::ZEXT_TRUNC_OR_BITCAST);
}

/**
 * UNPCKLPS/UNPCKHPS, UNPCKLPD/UNPCKHPD — interleave lanes from the two halves.
 */
static Value* sseUnpckI128(Value* a, Value* b, bool isDouble, bool high, IRBuilder<>& irb)
{
	Type* elemTy = isDouble ? irb.getDoubleTy() : irb.getFloatTy();
	unsigned n = isDouble ? 2 : 4;
	auto* vecTy = FixedVectorType::get(elemTy, n);
	Value* va = irb.CreateBitCast(toI128(a, irb), vecTy);
	Value* vb = irb.CreateBitCast(toI128(b, irb), vecTy);
	std::vector<int> mask;
	unsigned base = high ? n / 2 : 0;
	for (unsigned k = 0; k < n / 2; ++k)
	{
		mask.push_back(static_cast<int>(base + k));
		mask.push_back(static_cast<int>(n + base + k));
	}
	return irb.CreateBitCast(irb.CreateShuffleVector(va, vb, mask), irb.getInt128Ty());
}

void Capstone2LlvmIrTranslatorX86_impl::translateSseUnpck(cs_insn* i, cs_x86* xi, IRBuilder<>& irb)
{
	bool vex = (i->id == X86_INS_VUNPCKHPS || i->id == X86_INS_VUNPCKHPD
		|| i->id == X86_INS_VUNPCKLPS || i->id == X86_INS_VUNPCKLPD);
	bool isDouble = (i->id == X86_INS_UNPCKLPD || i->id == X86_INS_UNPCKHPD
		|| i->id == X86_INS_VUNPCKLPD || i->id == X86_INS_VUNPCKHPD);
	bool high = (i->id == X86_INS_UNPCKHPS || i->id == X86_INS_UNPCKHPD
		|| i->id == X86_INS_VUNPCKHPS || i->id == X86_INS_VUNPCKHPD);

	if (vex)
	{
		if (xi->op_count != 3)
		{
			translatePseudoAsmGeneric(i, xi, irb);
			return;
		}
		unsigned destBits = xi->operands[0].size * 8;
		if (destBits != 128 && destBits != 256)
		{
			translatePseudoAsmGeneric(i, xi, irb);
			return;
		}
		unsigned bits = destBits;
		Value* a = loadVectorOp(xi->operands[1], irb, bits);
		bits = destBits;
		Value* b = loadVectorOp(xi->operands[2], irb, bits);
		if (a == nullptr || b == nullptr)
		{
			translatePseudoAsmGeneric(i, xi, irb);
			return;
		}
		Value* result = nullptr;
		if (destBits == 128)
		{
			result = sseUnpckI128(a, b, isDouble, high, irb);
		}
		else
		{
			// AVX unpack is two independent 128-bit unpacks, one per lane.
			result = concatLane128(
				sseUnpckI128(extractLane128(a, 0, irb), extractLane128(b, 0, irb), isDouble, high, irb),
				sseUnpckI128(extractLane128(a, 1, irb), extractLane128(b, 1, irb), isDouble, high, irb),
				irb);
		}
		storeVectorOp(xi->operands[0], result, destBits, irb);
		return;
	}

	EXPECT_IS_BINARY(i, xi, irb);
	std::tie(op0, op1) = loadOpBinary(xi, irb, eOpConv::NOTHING);
	storeOp(xi->operands[0], sseUnpckI128(op0, op1, isDouble, high, irb), irb, eOpConv::NOTHING);
}

/**
 * SHUFPS, SHUFPD — the low half of the result is picked out of the
 * destination, the high half out of the source, both by the immediate.
 */
void Capstone2LlvmIrTranslatorX86_impl::translateSseShufp(cs_insn* i, cs_x86* xi, IRBuilder<>& irb)
{
	bool vex = (i->id == X86_INS_VSHUFPS || i->id == X86_INS_VSHUFPD);
	bool isDouble = i->id == X86_INS_SHUFPD || i->id == X86_INS_VSHUFPD;
	if (vex)
	{
		if (xi->op_count != 4 || xi->operands[3].type != X86_OP_IMM)
		{
			translatePseudoAsmGeneric(i, xi, irb);
			return;
		}
		unsigned destBits = xi->operands[0].size * 8;
		if (destBits != 128 && destBits != 256)
		{
			translatePseudoAsmGeneric(i, xi, irb);
			return;
		}
		unsigned bits = destBits;
		Value* a = loadVectorOp(xi->operands[1], irb, bits);
		bits = destBits;
		Value* b = loadVectorOp(xi->operands[2], irb, bits);
		if (a == nullptr || b == nullptr)
		{
			translatePseudoAsmGeneric(i, xi, irb);
			return;
		}
		auto ctrl = static_cast<uint8_t>(xi->operands[3].imm);
		auto shuf128 = [&](Value* va, Value* vb) {
			Type* elemTy = isDouble ? irb.getDoubleTy() : irb.getFloatTy();
			unsigned n = isDouble ? 2 : 4;
			auto* vecTy = FixedVectorType::get(elemTy, n);
			Value* fa = irb.CreateBitCast(irb.CreateZExtOrTrunc(va, irb.getInt128Ty()), vecTy);
			Value* fb = irb.CreateBitCast(irb.CreateZExtOrTrunc(vb, irb.getInt128Ty()), vecTy);
			std::vector<int> mask;
			if (isDouble)
			{
				mask = {static_cast<int>(ctrl & 1), static_cast<int>(2 + ((ctrl >> 1) & 1))};
			}
			else
			{
				mask = {
					static_cast<int>(ctrl & 3),
					static_cast<int>((ctrl >> 2) & 3),
					static_cast<int>(4 + ((ctrl >> 4) & 3)),
					static_cast<int>(4 + ((ctrl >> 6) & 3))};
			}
			return irb.CreateBitCast(irb.CreateShuffleVector(fa, fb, mask), irb.getInt128Ty());
		};
		Value* result = nullptr;
		if (destBits == 128)
		{
			result = shuf128(a, b);
		}
		else
		{
			a = irb.CreateZExtOrTrunc(a, irb.getIntNTy(256));
			b = irb.CreateZExtOrTrunc(b, irb.getIntNTy(256));
			result = concatLane128(
				shuf128(extractLane128(a, 0, irb), extractLane128(b, 0, irb)),
				shuf128(extractLane128(a, 1, irb), extractLane128(b, 1, irb)),
				irb);
		}
		storeVectorOp(xi->operands[0], result, destBits, irb);
		return;
	}

	EXPECT_IS_TERNARY(i, xi, irb);

	Type* elemTy = isDouble ? irb.getDoubleTy() : irb.getFloatTy();
	unsigned n = isDouble ? 2 : 4;
	auto* vecTy = FixedVectorType::get(elemTy, n);

	Value* a = irb.CreateBitCast(toI128(loadOp(xi->operands[0], irb), irb), vecTy);
	Value* b = irb.CreateBitCast(toI128(loadOp(xi->operands[1], irb), irb), vecTy);
	auto ctrl = static_cast<uint8_t>(xi->operands[2].imm);

	std::vector<int> mask;
	if (isDouble)
	{
		mask = {static_cast<int>(ctrl & 1), static_cast<int>(2 + ((ctrl >> 1) & 1))};
	}
	else
	{
		mask = {
			static_cast<int>(ctrl & 3),
			static_cast<int>((ctrl >> 2) & 3),
			static_cast<int>(4 + ((ctrl >> 4) & 3)),
			static_cast<int>(4 + ((ctrl >> 6) & 3))};
	}
	storeOp(
		xi->operands[0],
		irb.CreateBitCast(irb.CreateShuffleVector(a, b, mask), irb.getInt128Ty()),
		irb,
		eOpConv::NOTHING);
}

/**
 * MOVMSKPS, MOVMSKPD, PMOVMSKB — gather the lanes' sign bits into a GPR.
 *
 * Built out of extractelement and shifts rather than `bitcast <4 x i1> to i4`
 * so that it stays within what tests/llvmir-emul can execute.
 *
 * PMOVMSKB is the same operation over sixteen byte lanes, and it is the single
 * most frequent untranslated instruction in the static corpus -- 21,102
 * occurrences, 0.40% of everything decoded. That is not because anyone writes
 * it: it is the second half of glibc's SSE2 string routines, which compare
 * sixteen bytes at a time with PCMPEQB and then ask "which of those sixteen
 * matched" exactly this way. PCMPEQB was already translated, so the answer was
 * being computed and then thrown away at an __asm_pmovmskb call, and the TEST
 * and Jcc that follow it read a value out of nowhere.
 */
// Defined with the other vector-register predicates further down; needed here
// because MOVMSK's lane count comes from the source register's width.
static bool isYmmRegister(uint32_t r);
static bool isZmmRegister(uint32_t r);

void Capstone2LlvmIrTranslatorX86_impl::translateSseMovMsk(cs_insn* i, cs_x86* xi, IRBuilder<>& irb)
{
	EXPECT_IS_BINARY(i, xi, irb);

	bool isDouble = i->id == X86_INS_MOVMSKPD || i->id == X86_INS_VMOVMSKPD;
	bool isByte = i->id == X86_INS_PMOVMSKB;
	unsigned laneBits = isByte ? 8 : (isDouble ? 64 : 32);

	// The lane count comes from the SOURCE OPERAND's width, not from the
	// mnemonic. The VEX forms take a YMM source and then produce twice as
	// many bits: measured, `vmovmskps eax, ymm0` with all eight sign bits set
	// answers 0xff and `vmovmskpd eax, ymm0` answers 0x0f. This was hardwired
	// to the XMM width and then ran the i256 YMM read through toI128, which
	// TRUNCATES -- so the upper four floats were read and discarded, and the
	// answer was 0x0f and 0x03. translateAvxPmovmskb next door already
	// derives its width from isYmmRegister for exactly this reason.
	unsigned srcBits = 128;
	cs_x86_op& src = xi->operands[1];
	if (src.type == X86_OP_REG && isYmmRegister(src.reg))
		srcBits = 256;
	else if (src.type == X86_OP_REG && isZmmRegister(src.reg))
		srcBits = 512;

	unsigned n = srcBits / laneBits;
	auto* vecTy = FixedVectorType::get(irb.getIntNTy(laneBits), n);

	Value* raw = loadVectorOp(src, irb, srcBits);
	if (raw == nullptr)
	{
		translatePseudoAsmGeneric(i, xi, irb);
		return;
	}
	Value* v = irb.CreateBitCast(irb.CreateZExtOrTrunc(raw, irb.getIntNTy(srcBits)), vecTy);
	auto* i32t = irb.getInt32Ty();
	Value* res = ConstantInt::get(i32t, 0);
	for (unsigned lane = 0; lane < n; ++lane)
	{
		Value* e = irb.CreateExtractElement(v, (uint64_t)lane);
		Value* bit = irb.CreateZExt(
			irb.CreateTrunc(irb.CreateLShr(e, ConstantInt::get(e->getType(), laneBits - 1)), irb.getInt1Ty()), i32t);
		res = irb.CreateOr(res, irb.CreateShl(bit, ConstantInt::get(i32t, lane)));
	}
	storeOp(xi->operands[0], res, irb, eOpConv::ZEXT_TRUNC_OR_BITCAST);
}


/**
 * PMIN/PMAX, signed and unsigned, byte through dword.
 *
 * `select(a < b, a, b)` rather than an intrinsic, for the same reason
 * translateSseFltMinMax uses a select: these are exact integer comparisons
 * with no NaN case to get wrong, and the emulator executes select and icmp.
 *
 * Signedness is the whole instruction. PMINUB and PMINSB differ on every lane
 * whose top bit is set, which for bytes coming out of a PCMPEQB is all of the
 * matching ones, so reading the U or the S wrongly is a silent miscompilation
 * in exactly the code these appear in.
 */
/**
 * Clamp a widened vector into the range of a narrower element, elementwise.
 *
 * Both the signed and the unsigned forms clamp with SIGNED comparisons. For
 * the signed ones that is obvious. For the unsigned ones it is the point:
 * `psubusb` of 1 and 2 is a negative intermediate, and the widened value has
 * to be compared against zero as a signed number to notice. Widening to twice
 * the element gives room for every sum and difference of two N-bit values, so
 * the arithmetic itself can never overflow and only the clamp decides.
 */
static Value* clampToRange(Value* wide, unsigned bits, bool isSigned, IRBuilder<>& irb)
{
	Type* ty = wide->getType();
	unsigned wideBits = ty->getScalarType()->getIntegerBitWidth();
	APInt lo = isSigned ? APInt::getSignedMinValue(bits).sext(wideBits) : APInt::getZero(wideBits);
	APInt hi = isSigned ? APInt::getSignedMaxValue(bits).sext(wideBits) : APInt::getMaxValue(bits).zext(wideBits);
	auto* loC = ConstantInt::get(ty, lo);
	auto* hiC = ConstantInt::get(ty, hi);
	Value* v = irb.CreateSelect(irb.CreateICmpSLT(wide, loC), loC, wide);
	return irb.CreateSelect(irb.CreateICmpSGT(v, hiC), hiC, v);
}

/**
 * PADDSB/W, PADDUSB/W, PSUBSB/W, PSUBUSB/W -- add or subtract and stop at the
 * end of the range instead of wrapping.
 *
 * This is the whole reason they exist, and it is exactly what a plain vector
 * add gets wrong in a way that looks right: `paddsb` of 127 and 1 is 127, not
 * -128. A select is used rather than llvm.sadd.sat for the same reason
 * translateSsePminMax uses one -- the emitted C reads as arithmetic rather
 * than as a call to something the writer has to be taught.
 */
void Capstone2LlvmIrTranslatorX86_impl::translateSseSaturatingArith(cs_insn* i, cs_x86* xi, IRBuilder<>& irb)
{
	unsigned bits = 0;
	bool isSigned = true;
	bool isAdd = true;
	switch (i->id)
	{
	case X86_INS_PADDSB:
	case X86_INS_VPADDSB: bits = 8; break;
	case X86_INS_PADDSW:
	case X86_INS_VPADDSW: bits = 16; break;
	case X86_INS_PADDUSB:
	case X86_INS_VPADDUSB:
		bits = 8;
		isSigned = false;
		break;
	case X86_INS_PADDUSW:
	case X86_INS_VPADDUSW:
		bits = 16;
		isSigned = false;
		break;
	case X86_INS_PSUBSB:
	case X86_INS_VPSUBSB:
		bits = 8;
		isAdd = false;
		break;
	case X86_INS_PSUBSW:
	case X86_INS_VPSUBSW:
		bits = 16;
		isAdd = false;
		break;
	case X86_INS_PSUBUSB:
	case X86_INS_VPSUBUSB:
		bits = 8;
		isSigned = false;
		isAdd = false;
		break;
	case X86_INS_PSUBUSW:
	case X86_INS_VPSUBUSW:
		bits = 16;
		isSigned = false;
		isAdd = false;
		break;
	default: translatePseudoAsmGeneric(i, xi, irb); return;
	}

	bool vex = (i->id == X86_INS_VPADDSB || i->id == X86_INS_VPADDSW || i->id == X86_INS_VPADDUSB
		|| i->id == X86_INS_VPADDUSW || i->id == X86_INS_VPSUBSB || i->id == X86_INS_VPSUBSW
		|| i->id == X86_INS_VPSUBUSB || i->id == X86_INS_VPSUBUSW);
	if (vex)
	{
		if (xi->op_count != 3)
		{
			translatePseudoAsmGeneric(i, xi, irb);
			return;
		}
		unsigned destBits = xi->operands[0].size * 8;
		if (destBits != 128 && destBits != 256)
		{
			translatePseudoAsmGeneric(i, xi, irb);
			return;
		}
		unsigned loadBits = destBits;
		Value* aRaw = loadVectorOp(xi->operands[1], irb, loadBits);
		loadBits = destBits;
		Value* bRaw = loadVectorOp(xi->operands[2], irb, loadBits);
		if (aRaw == nullptr || bRaw == nullptr || destBits % bits != 0)
		{
			translatePseudoAsmGeneric(i, xi, irb);
			return;
		}
		unsigned n = destBits / bits;
		auto* intTy = irb.getIntNTy(destBits);
		auto* vecTy = vecType(irb.getIntNTy(bits), n);
		auto* wideTy = vecType(irb.getIntNTy(bits * 2), n);
		Value* a = irb.CreateBitCast(irb.CreateZExtOrTrunc(aRaw, intTy), vecTy);
		Value* b = irb.CreateBitCast(irb.CreateZExtOrTrunc(bRaw, intTy), vecTy);
		Value* aw = isSigned ? irb.CreateSExt(a, wideTy) : irb.CreateZExt(a, wideTy);
		Value* bw = isSigned ? irb.CreateSExt(b, wideTy) : irb.CreateZExt(b, wideTy);
		Value* r = isAdd ? irb.CreateAdd(aw, bw) : irb.CreateSub(aw, bw);
		r = irb.CreateTrunc(clampToRange(r, bits, isSigned, irb), vecTy);
		storeVectorOp(xi->operands[0], irb.CreateBitCast(r, intTy), destBits, irb);
		return;
	}

	EXPECT_IS_BINARY(i, xi, irb);

	unsigned totalBits = xi->operands[0].size * 8;
	if (totalBits < bits || totalBits % bits != 0)
	{
		translatePseudoAsmGeneric(i, xi, irb);
		return;
	}
	unsigned n = totalBits / bits;

	std::tie(op0, op1) = loadOpBinary(xi, irb, eOpConv::NOTHING);
	auto* intTy = irb.getIntNTy(totalBits);
	auto* vecTy = vecType(irb.getIntNTy(bits), n);
	auto* wideTy = vecType(irb.getIntNTy(bits * 2), n);

	Value* a = irb.CreateBitCast(irb.CreateZExtOrTrunc(toI128(op0, irb), intTy), vecTy);
	Value* b = irb.CreateBitCast(irb.CreateZExtOrTrunc(toI128(op1, irb), intTy), vecTy);

	Value* aw = isSigned ? irb.CreateSExt(a, wideTy) : irb.CreateZExt(a, wideTy);
	Value* bw = isSigned ? irb.CreateSExt(b, wideTy) : irb.CreateZExt(b, wideTy);
	Value* r = isAdd ? irb.CreateAdd(aw, bw) : irb.CreateSub(aw, bw);
	r = irb.CreateTrunc(clampToRange(r, bits, isSigned, irb), vecTy);

	storeOp(
		xi->operands[0], irb.CreateZExtOrTrunc(irb.CreateBitCast(r, intTy), irb.getInt128Ty()), irb, eOpConv::NOTHING);
}

/**
 * PACKSSWB/DW and PACKUSWB/DW -- halve the element width of two registers and
 * put the results side by side, saturating rather than truncating.
 *
 * The source is signed in every case, including the US forms: PACKUSWB
 * saturates a SIGNED word into an UNSIGNED byte, so -1 becomes 0 and 300
 * becomes 255. Reading the source as unsigned would make -1 into 255, which
 * is the same bit pattern arrived at backwards.
 *
 * The destination's low half comes from the first operand and the high half
 * from the second.
 */
void Capstone2LlvmIrTranslatorX86_impl::translateSsePack(cs_insn* i, cs_x86* xi, IRBuilder<>& irb)
{
	unsigned srcBits = 0;
	bool dstSigned = true;
	switch (i->id)
	{
	case X86_INS_PACKSSWB:
	case X86_INS_VPACKSSWB: srcBits = 16; break;
	case X86_INS_PACKSSDW:
	case X86_INS_VPACKSSDW: srcBits = 32; break;
	case X86_INS_PACKUSWB:
	case X86_INS_VPACKUSWB:
		srcBits = 16;
		dstSigned = false;
		break;
	case X86_INS_PACKUSDW:
	case X86_INS_VPACKUSDW:
		srcBits = 32;
		dstSigned = false;
		break;
	default: translatePseudoAsmGeneric(i, xi, irb); return;
	}

	auto packI128 = [&](Value* aIn, Value* bIn) {
		unsigned n = 128 / srcBits;
		unsigned dstBits = srcBits / 2;
		auto* intTy = irb.getInt128Ty();
		auto* srcVecTy = vecType(irb.getIntNTy(srcBits), n);
		auto* dstVecTy = vecType(irb.getIntNTy(dstBits), n);
		Value* a = irb.CreateBitCast(irb.CreateZExtOrTrunc(aIn, intTy), srcVecTy);
		Value* b = irb.CreateBitCast(irb.CreateZExtOrTrunc(bIn, intTy), srcVecTy);
		Value* sa = irb.CreateTrunc(clampToRange(a, dstBits, dstSigned, irb), dstVecTy);
		Value* sb = irb.CreateTrunc(clampToRange(b, dstBits, dstSigned, irb), dstVecTy);
		llvm::SmallVector<int, 32> mask;
		for (unsigned k = 0; k < 2 * n; ++k)
		{
			mask.push_back(static_cast<int>(k));
		}
		return irb.CreateBitCast(irb.CreateShuffleVector(sa, sb, mask), intTy);
	};

	bool vex = (i->id == X86_INS_VPACKSSWB || i->id == X86_INS_VPACKSSDW || i->id == X86_INS_VPACKUSWB
		|| i->id == X86_INS_VPACKUSDW);
	if (vex)
	{
		if (xi->op_count != 3)
		{
			translatePseudoAsmGeneric(i, xi, irb);
			return;
		}
		unsigned destBits = xi->operands[0].size * 8;
		if (destBits != 128 && destBits != 256)
		{
			translatePseudoAsmGeneric(i, xi, irb);
			return;
		}
		unsigned loadBits = destBits;
		Value* aRaw = loadVectorOp(xi->operands[1], irb, loadBits);
		loadBits = destBits;
		Value* bRaw = loadVectorOp(xi->operands[2], irb, loadBits);
		if (aRaw == nullptr || bRaw == nullptr)
		{
			translatePseudoAsmGeneric(i, xi, irb);
			return;
		}
		Value* result = nullptr;
		if (destBits == 128)
		{
			result = packI128(aRaw, bRaw);
		}
		else
		{
			aRaw = irb.CreateZExtOrTrunc(aRaw, irb.getIntNTy(256));
			bRaw = irb.CreateZExtOrTrunc(bRaw, irb.getIntNTy(256));
			result = concatLane128(
				packI128(extractLane128(aRaw, 0, irb), extractLane128(bRaw, 0, irb)),
				packI128(extractLane128(aRaw, 1, irb), extractLane128(bRaw, 1, irb)),
				irb);
		}
		storeVectorOp(xi->operands[0], result, destBits, irb);
		return;
	}

	EXPECT_IS_BINARY(i, xi, irb);

	unsigned totalBits = xi->operands[0].size * 8;
	if (totalBits % srcBits != 0)
	{
		translatePseudoAsmGeneric(i, xi, irb);
		return;
	}
	unsigned n = totalBits / srcBits;
	unsigned dstBits = srcBits / 2;

	std::tie(op0, op1) = loadOpBinary(xi, irb, eOpConv::NOTHING);
	auto* intTy = irb.getIntNTy(totalBits);
	auto* srcVecTy = vecType(irb.getIntNTy(srcBits), n);
	auto* dstVecTy = vecType(irb.getIntNTy(dstBits), n);

	Value* a = irb.CreateBitCast(irb.CreateZExtOrTrunc(toI128(op0, irb), intTy), srcVecTy);
	Value* b = irb.CreateBitCast(irb.CreateZExtOrTrunc(toI128(op1, irb), intTy), srcVecTy);

	Value* sa = irb.CreateTrunc(clampToRange(a, dstBits, dstSigned, irb), dstVecTy);
	Value* sb = irb.CreateTrunc(clampToRange(b, dstBits, dstSigned, irb), dstVecTy);

	llvm::SmallVector<int, 32> mask;
	for (unsigned k = 0; k < 2 * n; ++k)
	{
		mask.push_back(static_cast<int>(k));
	}
	Value* r = irb.CreateShuffleVector(sa, sb, mask);

	storeOp(
		xi->operands[0], irb.CreateZExtOrTrunc(irb.CreateBitCast(r, intTy), irb.getInt128Ty()), irb, eOpConv::NOTHING);
}

/**
 * PMULLW, PMULHW, PMULHUW, PMULLD, PMULUDQ, PMULDQ.
 *
 * Three different answers to "what do you keep":
 *
 *   the LOW half      PMULLW and PMULLD, where the product wraps into the
 *                     element and the signedness cannot be observed.
 *   the HIGH half     PMULHW and PMULHUW, where it is the only thing kept
 *                     and the signedness is the whole difference between
 *                     them.
 *   the WHOLE product PMULUDQ and PMULDQ, which read only the EVEN dwords --
 *                     lanes 0 and 2 -- and write two quadwords. The odd
 *                     dwords are not multiplied at all.
 */
void Capstone2LlvmIrTranslatorX86_impl::translateSsePackedMul(cs_insn* i, cs_x86* xi, IRBuilder<>& irb)
{
	unsigned bits = 0;
	bool isSigned = true;
	enum
	{
		LOW,
		HIGH,
		WHOLE_EVEN
	} keep = LOW;
	bool vex = false;
	switch (i->id)
	{
	case X86_INS_VPMULLW:
		vex = true;
		// fall through
	case X86_INS_PMULLW: bits = 16; break;
	case X86_INS_VPMULLD:
		vex = true;
		// fall through
	case X86_INS_PMULLD: bits = 32; break;
	case X86_INS_VPMULLQ:
		vex = true;
		bits = 64;
		break;
	case X86_INS_VPMULHW:
		vex = true;
		// fall through
	case X86_INS_PMULHW:
		bits = 16;
		keep = HIGH;
		break;
	case X86_INS_VPMULHUW:
		vex = true;
		// fall through
	case X86_INS_PMULHUW:
		bits = 16;
		keep = HIGH;
		isSigned = false;
		break;
	case X86_INS_VPMULDQ:
		vex = true;
		// fall through
	case X86_INS_PMULDQ:
		bits = 32;
		keep = WHOLE_EVEN;
		break;
	case X86_INS_VPMULUDQ:
		vex = true;
		// fall through
	case X86_INS_PMULUDQ:
		bits = 32;
		keep = WHOLE_EVEN;
		isSigned = false;
		break;
	default: translatePseudoAsmGeneric(i, xi, irb); return;
	}

	unsigned totalBits = xi->operands[0].size * 8;
	if ((vex && totalBits != 128 && totalBits != 256) || totalBits % bits != 0 || totalBits < bits * 2)
	{
		translatePseudoAsmGeneric(i, xi, irb);
		return;
	}
	unsigned n = totalBits / bits;
	Value* aRaw = nullptr;
	Value* bRaw = nullptr;
	if (vex)
	{
		if (xi->op_count != 3)
		{
			translatePseudoAsmGeneric(i, xi, irb);
			return;
		}
		aRaw = loadVectorOp(xi->operands[1], irb, totalBits);
		bRaw = loadVectorOp(xi->operands[2], irb, totalBits);
		if (aRaw == nullptr || bRaw == nullptr)
		{
			translatePseudoAsmGeneric(i, xi, irb);
			return;
		}
	}
	else
	{
		EXPECT_IS_BINARY(i, xi, irb);
		std::tie(op0, op1) = loadOpBinary(xi, irb, eOpConv::NOTHING);
		aRaw = toI128(op0, irb);
		bRaw = toI128(op1, irb);
	}

	auto* intTy = irb.getIntNTy(totalBits);
	auto* vecTy = vecType(irb.getIntNTy(bits), n);
	Value* a = irb.CreateBitCast(irb.CreateZExtOrTrunc(aRaw, intTy), vecTy);
	Value* b = irb.CreateBitCast(irb.CreateZExtOrTrunc(bRaw, intTy), vecTy);

	Value* r = nullptr;
	if (keep == LOW)
	{
		r = irb.CreateMul(a, b);
	}
	else if (keep == HIGH)
	{
		auto* wideTy = vecType(irb.getIntNTy(bits * 2), n);
		Value* aw = isSigned ? irb.CreateSExt(a, wideTy) : irb.CreateZExt(a, wideTy);
		Value* bw = isSigned ? irb.CreateSExt(b, wideTy) : irb.CreateZExt(b, wideTy);
		Value* prod = irb.CreateMul(aw, bw);
		Value* shifted = irb.CreateLShr(prod, ConstantInt::get(wideTy, APInt(bits * 2, bits)));
		r = irb.CreateTrunc(shifted, vecTy);
	}
	else
	{
		llvm::SmallVector<int, 8> even;
		for (unsigned k = 0; k < n; k += 2)
		{
			even.push_back(static_cast<int>(k));
		}
		auto* halfTy = vecType(irb.getIntNTy(bits), even.size());
		auto* wideTy = vecType(irb.getIntNTy(bits * 2), even.size());
		Value* ae = irb.CreateShuffleVector(a, llvm::UndefValue::get(vecTy), even);
		Value* be = irb.CreateShuffleVector(b, llvm::UndefValue::get(vecTy), even);
		(void)halfTy;
		Value* aw = isSigned ? irb.CreateSExt(ae, wideTy) : irb.CreateZExt(ae, wideTy);
		Value* bw = isSigned ? irb.CreateSExt(be, wideTy) : irb.CreateZExt(be, wideTy);
		r = irb.CreateMul(aw, bw);
	}

	Value* asInt = irb.CreateBitCast(r, keep == WHOLE_EVEN ? irb.getIntNTy(totalBits) : intTy);
	if (vex)
	{
		storeVectorOp(xi->operands[0], asInt, totalBits, irb);
	}
	else
	{
		storeOp(xi->operands[0], irb.CreateZExtOrTrunc(asInt, irb.getInt128Ty()), irb, eOpConv::NOTHING);
	}
}

/**
 * PMADDWD -- multiply signed words and add ADJACENT pairs of the products.
 *
 * dest[31:0] is a[15:0]*b[15:0] + a[31:16]*b[31:16], so the eight products
 * become four sums, not eight. The one input where the sum itself overflows
 * is -32768 squared twice, which wraps to 0x80000000; that is the defined
 * answer, so the add is left to wrap.
 */
void Capstone2LlvmIrTranslatorX86_impl::translateSsePmaddwd(cs_insn* i, cs_x86* xi, IRBuilder<>& irb)
{
	bool vex = (i->id == X86_INS_VPMADDWD);
	unsigned totalBits = xi->operands[0].size * 8;
	if ((vex && totalBits != 128 && totalBits != 256) || totalBits % 32 != 0)
	{
		translatePseudoAsmGeneric(i, xi, irb);
		return;
	}
	unsigned n = totalBits / 16;
	Value* aRaw = nullptr;
	Value* bRaw = nullptr;
	if (vex)
	{
		if (xi->op_count != 3)
		{
			translatePseudoAsmGeneric(i, xi, irb);
			return;
		}
		aRaw = loadVectorOp(xi->operands[1], irb, totalBits);
		bRaw = loadVectorOp(xi->operands[2], irb, totalBits);
		if (aRaw == nullptr || bRaw == nullptr)
		{
			translatePseudoAsmGeneric(i, xi, irb);
			return;
		}
	}
	else
	{
		EXPECT_IS_BINARY(i, xi, irb);
		std::tie(op0, op1) = loadOpBinary(xi, irb, eOpConv::NOTHING);
		aRaw = toI128(op0, irb);
		bRaw = toI128(op1, irb);
	}
	auto* intTy = irb.getIntNTy(totalBits);
	auto* vecTy = vecType(irb.getInt16Ty(), n);
	auto* wideTy = vecType(irb.getInt32Ty(), n);

	Value* a = irb.CreateBitCast(irb.CreateZExtOrTrunc(aRaw, intTy), vecTy);
	Value* b = irb.CreateBitCast(irb.CreateZExtOrTrunc(bRaw, intTy), vecTy);
	Value* prod = irb.CreateMul(irb.CreateSExt(a, wideTy), irb.CreateSExt(b, wideTy));

	llvm::SmallVector<int, 8> even, odd;
	for (unsigned k = 0; k < n; k += 2)
	{
		even.push_back(static_cast<int>(k));
		odd.push_back(static_cast<int>(k + 1));
	}
	Value* e = irb.CreateShuffleVector(prod, llvm::UndefValue::get(wideTy), even);
	Value* o = irb.CreateShuffleVector(prod, llvm::UndefValue::get(wideTy), odd);
	Value* r = irb.CreateAdd(e, o);
	Value* asInt = irb.CreateBitCast(r, intTy);
	if (vex)
	{
		storeVectorOp(xi->operands[0], asInt, totalBits, irb);
	}
	else
	{
		storeOp(xi->operands[0], irb.CreateZExtOrTrunc(asInt, irb.getInt128Ty()), irb, eOpConv::NOTHING);
	}
}

/**
 * PSADBW -- absolute differences of unsigned bytes, summed in groups of eight.
 *
 * Each group of eight bytes produces ONE number, written to the low word of
 * that group's quadword with the rest zeroed. Eight differences of bytes can
 * reach 2040, so the sum is accumulated at sixteen bits and cannot overflow.
 */
void Capstone2LlvmIrTranslatorX86_impl::translateSsePsadbw(cs_insn* i, cs_x86* xi, IRBuilder<>& irb)
{
	bool vex = i->id == X86_INS_VPSADBW;
	unsigned totalBits = xi->operands[0].size * 8;
	if (totalBits % 64 != 0 || (totalBits != 128 && totalBits != 256))
	{
		translatePseudoAsmGeneric(i, xi, irb);
		return;
	}
	unsigned groups = totalBits / 64;
	unsigned n = totalBits / 8;
	Value* aRaw = nullptr;
	Value* bRaw = nullptr;
	if (vex)
	{
		if (xi->op_count != 3)
		{
			translatePseudoAsmGeneric(i, xi, irb);
			return;
		}
		unsigned bits = totalBits;
		aRaw = loadVectorOp(xi->operands[1], irb, bits);
		bits = totalBits;
		bRaw = loadVectorOp(xi->operands[2], irb, bits);
		if (aRaw == nullptr || bRaw == nullptr)
		{
			translatePseudoAsmGeneric(i, xi, irb);
			return;
		}
	}
	else
	{
		EXPECT_IS_BINARY(i, xi, irb);
		std::tie(op0, op1) = loadOpBinary(xi, irb, eOpConv::NOTHING);
		aRaw = toI128(op0, irb);
		bRaw = toI128(op1, irb);
	}
	auto* intTy = irb.getIntNTy(totalBits);
	auto* vecTy = vecType(irb.getInt8Ty(), n);
	auto* wideTy = vecType(irb.getInt16Ty(), n);

	Value* a = irb.CreateBitCast(irb.CreateZExtOrTrunc(aRaw, intTy), vecTy);
	Value* b = irb.CreateBitCast(irb.CreateZExtOrTrunc(bRaw, intTy), vecTy);

	// Unsigned bytes widened, so the difference is exact and its sign is the
	// thing the absolute value is taken of.
	Value* d = irb.CreateSub(irb.CreateZExt(a, wideTy), irb.CreateZExt(b, wideTy));
	Value* neg = irb.CreateNeg(d);
	Value* absd = irb.CreateSelect(irb.CreateICmpSLT(d, Constant::getNullValue(wideTy)), neg, d);

	Value* res = ConstantInt::get(intTy, 0);
	for (unsigned g = 0; g < groups; ++g)
	{
		Value* sum = ConstantInt::get(irb.getInt16Ty(), 0);
		for (unsigned k = 0; k < 8; ++k)
		{
			sum = irb.CreateAdd(sum, irb.CreateExtractElement(absd, (uint64_t)(g * 8 + k)));
		}
		Value* placed = irb.CreateShl(irb.CreateZExt(sum, intTy), ConstantInt::get(intTy, g * 64));
		res = irb.CreateOr(res, placed);
	}

	if (vex)
	{
		storeVectorOp(xi->operands[0], res, totalBits, irb);
	}
	else
	{
		storeOp(xi->operands[0], res, irb, eOpConv::NOTHING);
	}
}

/**
 * PSHUFB -- permute bytes by a control vector held in the second operand.
 *
 * The control is data, not an immediate, so the index is only known at run
 * time and `shufflevector` cannot express it -- its mask has to be constant.
 * `extractelement` with a dynamic index can, which is what this uses.
 *
 * The rule has a branch in it that a plain permutation does not: a control
 * byte with its TOP BIT set writes zero rather than selecting a lane, and
 * only the low four bits of the rest are an index. A translation that masks
 * with 0x0f and forgets the top bit answers with a byte from the source
 * everywhere the hardware answers zero.
 */
void Capstone2LlvmIrTranslatorX86_impl::translateSsePshufb(cs_insn* i, cs_x86* xi, IRBuilder<>& irb)
{
	// SSE is dest/src + control. VEX is dest, src, control -- Capstone always
	// reports three operands for VPSHUFB, even when gcc wrote `vpshufb xmm0,
	// xmm1` and the assembler expanded dest into vvvv. Reusing the SSE path
	// without that extra index would permute the destination against itself.
	bool vex = i->id == X86_INS_VPSHUFB;
	if (vex)
	{
		if (xi->op_count != 3)
		{
			translatePseudoAsmGeneric(i, xi, irb);
			return;
		}
	}
	else
	{
		EXPECT_IS_BINARY(i, xi, irb);
	}

	unsigned totalBits = xi->operands[0].size * 8;
	unsigned n = totalBits / 8;
	if (totalBits % 8 != 0 || (n != 8 && n != 16 && n != 32))
	{
		translatePseudoAsmGeneric(i, xi, irb);
		return;
	}

	Value* srcRaw = nullptr;
	Value* ctlRaw = nullptr;
	if (vex)
	{
		unsigned srcBits = totalBits;
		srcRaw = loadVectorOp(xi->operands[1], irb, srcBits);
		unsigned ctlBits = totalBits;
		ctlRaw = loadVectorOp(xi->operands[2], irb, ctlBits);
		if (srcRaw == nullptr || ctlRaw == nullptr)
		{
			translatePseudoAsmGeneric(i, xi, irb);
			return;
		}
	}
	else
	{
		std::tie(op0, op1) = loadOpBinary(xi, irb, eOpConv::NOTHING);
		srcRaw = toI128(op0, irb);
		ctlRaw = toI128(op1, irb);
	}

	auto* intTy = irb.getIntNTy(totalBits);
	auto* vecTy = vecType(irb.getInt8Ty(), n);
	Value* src = irb.CreateBitCast(irb.CreateZExtOrTrunc(srcRaw, intTy), vecTy);
	Value* ctl = irb.CreateBitCast(irb.CreateZExtOrTrunc(ctlRaw, intTy), vecTy);

	auto* i8 = irb.getInt8Ty();
	// AVX2 VPSHUFB is two independent 16-byte permutes, not one 32-byte one.
	// Masking with n-1 on a YMM operand would let a control byte select from
	// the other 128-bit half, which the hardware never does.
	unsigned laneBytes = n <= 16 ? n : 16;
	auto* idxMask = ConstantInt::get(i8, laneBytes - 1);
	Value* res = llvm::UndefValue::get(vecTy);
	for (unsigned lane = 0; lane < n; ++lane)
	{
		Value* c = irb.CreateExtractElement(ctl, (uint64_t)lane);
		unsigned base = (lane / laneBytes) * laneBytes;
		Value* idx = irb.CreateAdd(irb.CreateAnd(c, idxMask), ConstantInt::get(i8, base));
		Value* picked = irb.CreateExtractElement(src, idx);
		Value* zeroIt = irb.CreateICmpNE(irb.CreateAnd(c, ConstantInt::get(i8, 0x80)), ConstantInt::get(i8, 0));
		Value* v = irb.CreateSelect(zeroIt, ConstantInt::get(i8, 0), picked);
		res = irb.CreateInsertElement(res, v, (uint64_t)lane);
	}

	Value* asInt = irb.CreateBitCast(res, intTy);
	if (vex)
	{
		storeVectorOp(xi->operands[0], asInt, totalBits, irb);
	}
	else
	{
		storeOp(xi->operands[0], irb.CreateZExtOrTrunc(asInt, irb.getInt128Ty()), irb, eOpConv::NOTHING);
	}
}

/**
 * PHADDW/D/SW and PHSUBW/D/SW -- add or subtract ADJACENT pairs within each
 * operand, rather than corresponding lanes across the two.
 *
 * The destination's low half comes from the first operand's pairs and its
 * high half from the second's, so this is two independent reductions written
 * side by side. The SW forms saturate; the others wrap.
 *
 * PHSUB subtracts the SECOND element of each pair from the first -- a[0]-a[1],
 * not a[1]-a[0].
 */
void Capstone2LlvmIrTranslatorX86_impl::translateSseHorizontalInt(cs_insn* i, cs_x86* xi, IRBuilder<>& irb)
{
	unsigned bits = 0;
	bool isAdd = true;
	bool saturating = false;
	bool vex = false;
	switch (i->id)
	{
	case X86_INS_VPHADDW:
		vex = true;
		// fall through
	case X86_INS_PHADDW: bits = 16; break;
	case X86_INS_VPHADDD:
		vex = true;
		// fall through
	case X86_INS_PHADDD: bits = 32; break;
	case X86_INS_VPHADDSW:
		vex = true;
		// fall through
	case X86_INS_PHADDSW:
		bits = 16;
		saturating = true;
		break;
	case X86_INS_VPHSUBW:
		vex = true;
		// fall through
	case X86_INS_PHSUBW:
		bits = 16;
		isAdd = false;
		break;
	case X86_INS_VPHSUBD:
		vex = true;
		// fall through
	case X86_INS_PHSUBD:
		bits = 32;
		isAdd = false;
		break;
	case X86_INS_VPHSUBSW:
		vex = true;
		// fall through
	case X86_INS_PHSUBSW:
		bits = 16;
		isAdd = false;
		saturating = true;
		break;
	default: translatePseudoAsmGeneric(i, xi, irb); return;
	}

	// AVX2 VPHADD/VPHSUB is two independent 128-bit reductions, not one
	// 256-bit pairwise across the whole YMM.
	auto hadd128 = [&](Value* aRaw, Value* bRaw) -> Value* {
		const unsigned totalBits = 128;
		unsigned n = totalBits / bits;
		unsigned half = n / 2;
		auto* intTy = irb.getIntNTy(totalBits);
		auto* vecTy = vecType(irb.getIntNTy(bits), n);
		auto* halfTy = vecType(irb.getIntNTy(bits), half);
		auto* wideTy = vecType(irb.getIntNTy(bits * 2), half);
		Value* a = irb.CreateBitCast(irb.CreateZExtOrTrunc(toI128(aRaw, irb), intTy), vecTy);
		Value* b = irb.CreateBitCast(irb.CreateZExtOrTrunc(toI128(bRaw, irb), intTy), vecTy);
		llvm::SmallVector<int, 8> even, odd;
		for (unsigned k = 0; k < n; k += 2)
		{
			even.push_back(static_cast<int>(k));
			odd.push_back(static_cast<int>(k + 1));
		}
		auto pairwise = [&](Value* v) -> Value* {
			Value* e = irb.CreateShuffleVector(v, llvm::UndefValue::get(vecTy), even);
			Value* o = irb.CreateShuffleVector(v, llvm::UndefValue::get(vecTy), odd);
			if (!saturating)
			{
				return isAdd ? irb.CreateAdd(e, o) : irb.CreateSub(e, o);
			}
			Value* ew = irb.CreateSExt(e, wideTy);
			Value* ow = irb.CreateSExt(o, wideTy);
			Value* r = isAdd ? irb.CreateAdd(ew, ow) : irb.CreateSub(ew, ow);
			return irb.CreateTrunc(clampToRange(r, bits, true, irb), halfTy);
		};
		Value* ra = pairwise(a);
		Value* rb = pairwise(b);
		llvm::SmallVector<int, 16> cat;
		for (unsigned k = 0; k < n; ++k)
		{
			cat.push_back(static_cast<int>(k));
		}
		return irb.CreateBitCast(irb.CreateShuffleVector(ra, rb, cat), intTy);
	};

	if (vex)
	{
		if (xi->op_count != 3)
		{
			translatePseudoAsmGeneric(i, xi, irb);
			return;
		}
		unsigned destBits = xi->operands[0].size * 8;
		if (destBits != 128 && destBits != 256)
		{
			translatePseudoAsmGeneric(i, xi, irb);
			return;
		}
		Value* a = loadVectorOp(xi->operands[1], irb, destBits);
		Value* b = loadVectorOp(xi->operands[2], irb, destBits);
		if (a == nullptr || b == nullptr)
		{
			translatePseudoAsmGeneric(i, xi, irb);
			return;
		}
		Value* result = destBits == 128
			? hadd128(a, b)
			: concatLane128(
				hadd128(extractLane128(a, 0, irb), extractLane128(b, 0, irb)),
				hadd128(extractLane128(a, 1, irb), extractLane128(b, 1, irb)),
				irb);
		storeVectorOp(xi->operands[0], result, destBits, irb);
		return;
	}

	EXPECT_IS_BINARY(i, xi, irb);
	std::tie(op0, op1) = loadOpBinary(xi, irb, eOpConv::NOTHING);
	storeOp(xi->operands[0], hadd128(op0, op1), irb, eOpConv::NOTHING);
}

/**
 * PABSB/W/D -- elementwise absolute value.
 *
 * The minimum signed value has no positive counterpart, and x86 answers with
 * it unchanged rather than saturating: `pabsb` of -128 is -128. Negating and
 * letting it wrap is exactly that, so no special case is needed -- but a
 * translation that saturated to 127 "to be safe" would be wrong.
 */
void Capstone2LlvmIrTranslatorX86_impl::translateSsePabs(cs_insn* i, cs_x86* xi, IRBuilder<>& irb)
{
	unsigned bits = 0;
	switch (i->id)
	{
	case X86_INS_PABSB:
	case X86_INS_VPABSB: bits = 8; break;
	case X86_INS_PABSW:
	case X86_INS_VPABSW: bits = 16; break;
	case X86_INS_PABSD:
	case X86_INS_VPABSD: bits = 32; break;
	case X86_INS_VPABSQ: bits = 64; break;
	default: translatePseudoAsmGeneric(i, xi, irb); return;
	}

	bool vex = (i->id == X86_INS_VPABSB || i->id == X86_INS_VPABSW || i->id == X86_INS_VPABSD
		|| i->id == X86_INS_VPABSQ);
	if (vex)
	{
		EXPECT_IS_BINARY(i, xi, irb);
		unsigned destBits = xi->operands[0].size * 8;
		if (destBits != 128 && destBits != 256)
		{
			translatePseudoAsmGeneric(i, xi, irb);
			return;
		}
		unsigned loadBits = destBits;
		Value* src = loadVectorOp(xi->operands[1], irb, loadBits);
		if (src == nullptr || destBits % bits != 0)
		{
			translatePseudoAsmGeneric(i, xi, irb);
			return;
		}
		unsigned n = destBits / bits;
		auto* intTy = irb.getIntNTy(destBits);
		auto* vecTy = vecType(irb.getIntNTy(bits), n);
		Value* v = irb.CreateBitCast(irb.CreateZExtOrTrunc(src, intTy), vecTy);
		Value* neg = irb.CreateNeg(v);
		Value* res = irb.CreateSelect(irb.CreateICmpSLT(v, Constant::getNullValue(vecTy)), neg, v);
		storeVectorOp(xi->operands[0], irb.CreateBitCast(res, intTy), destBits, irb);
		return;
	}

	EXPECT_IS_BINARY(i, xi, irb);

	unsigned totalBits = xi->operands[0].size * 8;
	if (totalBits % bits != 0)
	{
		translatePseudoAsmGeneric(i, xi, irb);
		return;
	}
	unsigned n = totalBits / bits;

	op1 = loadOpBinaryOp1(xi, irb);
	auto* intTy = irb.getIntNTy(totalBits);
	auto* vecTy = vecType(irb.getIntNTy(bits), n);
	Value* v = irb.CreateBitCast(irb.CreateZExtOrTrunc(toI128(op1, irb), intTy), vecTy);

	Value* neg = irb.CreateNeg(v);
	Value* res = irb.CreateSelect(irb.CreateICmpSLT(v, Constant::getNullValue(vecTy)), neg, v);

	storeOp(
		xi->operands[0],
		irb.CreateZExtOrTrunc(irb.CreateBitCast(res, intTy), irb.getInt128Ty()),
		irb,
		eOpConv::NOTHING);
}

/**
 * PSIGNB/W/D -- apply the sign of the second operand to the first.
 *
 * Three outcomes per lane, not two: a negative control negates, a positive
 * one keeps, and a control of exactly ZERO writes zero. The zero case is the
 * one a `negative ? -a : a` translation silently drops.
 */
void Capstone2LlvmIrTranslatorX86_impl::translateSsePsign(cs_insn* i, cs_x86* xi, IRBuilder<>& irb)
{
	unsigned bits = 0;
	bool vex = false;
	switch (i->id)
	{
	case X86_INS_VPSIGNB:
		vex = true;
		// fall through
	case X86_INS_PSIGNB: bits = 8; break;
	case X86_INS_VPSIGNW:
		vex = true;
		// fall through
	case X86_INS_PSIGNW: bits = 16; break;
	case X86_INS_VPSIGND:
		vex = true;
		// fall through
	case X86_INS_PSIGND: bits = 32; break;
	default: translatePseudoAsmGeneric(i, xi, irb); return;
	}

	unsigned totalBits = xi->operands[0].size * 8;
	if ((vex && totalBits != 128 && totalBits != 256) || totalBits % bits != 0)
	{
		translatePseudoAsmGeneric(i, xi, irb);
		return;
	}
	unsigned n = totalBits / bits;
	Value* aRaw = nullptr;
	Value* bRaw = nullptr;
	if (vex)
	{
		if (xi->op_count != 3)
		{
			translatePseudoAsmGeneric(i, xi, irb);
			return;
		}
		aRaw = loadVectorOp(xi->operands[1], irb, totalBits);
		bRaw = loadVectorOp(xi->operands[2], irb, totalBits);
		if (aRaw == nullptr || bRaw == nullptr)
		{
			translatePseudoAsmGeneric(i, xi, irb);
			return;
		}
	}
	else
	{
		EXPECT_IS_BINARY(i, xi, irb);
		std::tie(op0, op1) = loadOpBinary(xi, irb, eOpConv::NOTHING);
		aRaw = toI128(op0, irb);
		bRaw = toI128(op1, irb);
	}

	auto* intTy = irb.getIntNTy(totalBits);
	auto* vecTy = vecType(irb.getIntNTy(bits), n);
	Value* a = irb.CreateBitCast(irb.CreateZExtOrTrunc(aRaw, intTy), vecTy);
	Value* b = irb.CreateBitCast(irb.CreateZExtOrTrunc(bRaw, intTy), vecTy);

	auto* zero = Constant::getNullValue(vecTy);
	Value* res = irb.CreateSelect(
		irb.CreateICmpSLT(b, zero), irb.CreateNeg(a), irb.CreateSelect(irb.CreateICmpEQ(b, zero), zero, a));
	Value* asInt = irb.CreateBitCast(res, intTy);
	if (vex)
	{
		storeVectorOp(xi->operands[0], asInt, totalBits, irb);
	}
	else
	{
		storeOp(xi->operands[0], irb.CreateZExtOrTrunc(asInt, irb.getInt128Ty()), irb, eOpConv::NOTHING);
	}
}

/**
 * PMULHRSW -- multiply signed words, keep the high half, ROUNDED rather than
 * truncated.
 *
 * `(a*b >> 14) + 1 >> 1` is the SDM's own formulation and it is not the same
 * as taking the high sixteen bits: the +1 before the final shift is what
 * rounds a half away from zero instead of down. Dropping it is off by one on
 * every product whose bit 14 is set.
 */
void Capstone2LlvmIrTranslatorX86_impl::translateSsePmulhrsw(cs_insn* i, cs_x86* xi, IRBuilder<>& irb)
{
	bool vex = (i->id == X86_INS_VPMULHRSW);
	unsigned totalBits = xi->operands[0].size * 8;
	if ((vex && totalBits != 128 && totalBits != 256) || totalBits % 16 != 0)
	{
		translatePseudoAsmGeneric(i, xi, irb);
		return;
	}
	unsigned n = totalBits / 16;
	Value* aRaw = nullptr;
	Value* bRaw = nullptr;
	if (vex)
	{
		if (xi->op_count != 3)
		{
			translatePseudoAsmGeneric(i, xi, irb);
			return;
		}
		aRaw = loadVectorOp(xi->operands[1], irb, totalBits);
		bRaw = loadVectorOp(xi->operands[2], irb, totalBits);
		if (aRaw == nullptr || bRaw == nullptr)
		{
			translatePseudoAsmGeneric(i, xi, irb);
			return;
		}
	}
	else
	{
		EXPECT_IS_BINARY(i, xi, irb);
		std::tie(op0, op1) = loadOpBinary(xi, irb, eOpConv::NOTHING);
		aRaw = toI128(op0, irb);
		bRaw = toI128(op1, irb);
	}

	auto* intTy = irb.getIntNTy(totalBits);
	auto* vecTy = vecType(irb.getInt16Ty(), n);
	auto* wideTy = vecType(irb.getInt32Ty(), n);

	Value* a = irb.CreateBitCast(irb.CreateZExtOrTrunc(aRaw, intTy), vecTy);
	Value* b = irb.CreateBitCast(irb.CreateZExtOrTrunc(bRaw, intTy), vecTy);

	Value* prod = irb.CreateMul(irb.CreateSExt(a, wideTy), irb.CreateSExt(b, wideTy));
	Value* r = irb.CreateAShr(prod, ConstantInt::get(wideTy, APInt(32, 14)));
	r = irb.CreateAdd(r, ConstantInt::get(wideTy, APInt(32, 1)));
	r = irb.CreateAShr(r, ConstantInt::get(wideTy, APInt(32, 1)));
	Value* asInt = irb.CreateBitCast(irb.CreateTrunc(r, vecTy), intTy);
	if (vex)
	{
		storeVectorOp(xi->operands[0], asInt, totalBits, irb);
	}
	else
	{
		storeOp(xi->operands[0], irb.CreateZExtOrTrunc(asInt, irb.getInt128Ty()), irb, eOpConv::NOTHING);
	}
}

/**
 * PMADDUBSW -- multiply UNSIGNED bytes by SIGNED bytes and add adjacent pairs,
 * saturating.
 *
 * The asymmetry is the instruction: the first operand's bytes are unsigned and
 * the second's are signed, so widening both the same way is wrong whichever
 * way is picked. The pair sum is then saturated to a signed word, which the
 * ordinary PMADDWD does not do.
 */
void Capstone2LlvmIrTranslatorX86_impl::translateSsePmaddubsw(cs_insn* i, cs_x86* xi, IRBuilder<>& irb)
{
	bool vex = (i->id == X86_INS_VPMADDUBSW);
	unsigned totalBits = xi->operands[0].size * 8;
	if ((vex && totalBits != 128 && totalBits != 256) || totalBits % 16 != 0)
	{
		translatePseudoAsmGeneric(i, xi, irb);
		return;
	}
	unsigned n = totalBits / 8;
	unsigned half = n / 2;
	Value* aRaw = nullptr;
	Value* bRaw = nullptr;
	if (vex)
	{
		if (xi->op_count != 3)
		{
			translatePseudoAsmGeneric(i, xi, irb);
			return;
		}
		aRaw = loadVectorOp(xi->operands[1], irb, totalBits);
		bRaw = loadVectorOp(xi->operands[2], irb, totalBits);
		if (aRaw == nullptr || bRaw == nullptr)
		{
			translatePseudoAsmGeneric(i, xi, irb);
			return;
		}
	}
	else
	{
		EXPECT_IS_BINARY(i, xi, irb);
		std::tie(op0, op1) = loadOpBinary(xi, irb, eOpConv::NOTHING);
		aRaw = toI128(op0, irb);
		bRaw = toI128(op1, irb);
	}
	auto* intTy = irb.getIntNTy(totalBits);
	auto* byteTy = vecType(irb.getInt8Ty(), n);
	auto* wordTy = vecType(irb.getInt16Ty(), n);
	auto* halfWordTy = vecType(irb.getInt16Ty(), half);
	auto* dwordTy = vecType(irb.getInt32Ty(), half);

	Value* a = irb.CreateBitCast(irb.CreateZExtOrTrunc(aRaw, intTy), byteTy);
	Value* b = irb.CreateBitCast(irb.CreateZExtOrTrunc(bRaw, intTy), byteTy);

	// Unsigned first, signed second. A product of u8 and s8 spans
	// [-32640, 32385], which a word holds, so only the pair SUM can overflow.
	Value* prod = irb.CreateMul(irb.CreateZExt(a, wordTy), irb.CreateSExt(b, wordTy));

	llvm::SmallVector<int, 8> even, odd;
	for (unsigned k = 0; k < n; k += 2)
	{
		even.push_back(static_cast<int>(k));
		odd.push_back(static_cast<int>(k + 1));
	}
	Value* e = irb.CreateShuffleVector(prod, llvm::UndefValue::get(wordTy), even);
	Value* o = irb.CreateShuffleVector(prod, llvm::UndefValue::get(wordTy), odd);
	Value* sum = irb.CreateAdd(irb.CreateSExt(e, dwordTy), irb.CreateSExt(o, dwordTy));
	Value* res = irb.CreateTrunc(clampToRange(sum, 16, true, irb), halfWordTy);
	Value* asInt = irb.CreateBitCast(res, intTy);
	if (vex)
	{
		storeVectorOp(xi->operands[0], asInt, totalBits, irb);
	}
	else
	{
		storeOp(xi->operands[0], irb.CreateZExtOrTrunc(asInt, irb.getInt128Ty()), irb, eOpConv::NOTHING);
	}
}

/**
 * PHMINPOSUW -- the smallest unsigned word and where it was.
 *
 * The value goes to bits 15:0 and its INDEX to bits 18:16; everything above
 * is zeroed. Ties take the lowest index, which falls out of scanning upward
 * with a strict comparison -- using `<=` instead would answer with the last
 * of a tie rather than the first.
 */
void Capstone2LlvmIrTranslatorX86_impl::translateSsePhminposuw(cs_insn* i, cs_x86* xi, IRBuilder<>& irb)
{
	EXPECT_IS_BINARY(i, xi, irb);

	unsigned totalBits = xi->operands[0].size * 8;
	if (totalBits != 128)
	{
		translatePseudoAsmGeneric(i, xi, irb);
		return;
	}

	op1 = (i->id == X86_INS_VPHMINPOSUW) ? loadOp(xi->operands[1], irb) : loadOpBinaryOp1(xi, irb);
	auto* vecTy = vecType(irb.getInt16Ty(), 8);
	Value* v = irb.CreateBitCast(toI128(op1, irb), vecTy);

	auto* i16 = irb.getInt16Ty();
	Value* best = irb.CreateExtractElement(v, (uint64_t)0);
	Value* bestIdx = ConstantInt::get(i16, 0);
	for (unsigned k = 1; k < 8; ++k)
	{
		Value* cur = irb.CreateExtractElement(v, (uint64_t)k);
		Value* lt = irb.CreateICmpULT(cur, best);
		best = irb.CreateSelect(lt, cur, best);
		bestIdx = irb.CreateSelect(lt, ConstantInt::get(i16, k), bestIdx);
	}

	auto* i128 = irb.getInt128Ty();
	Value* res = irb.CreateOr(
		irb.CreateZExt(best, i128), irb.CreateShl(irb.CreateZExt(bestIdx, i128), ConstantInt::get(i128, 16)));

	if (i->id == X86_INS_VPHMINPOSUW)
	{
		storeVectorOp(xi->operands[0], res, 128, irb);
	}
	else
	{
		storeOp(xi->operands[0], res, irb, eOpConv::NOTHING);
	}
}

/**
 * PSHUFHW and PSHUFLW -- permute half the register by an immediate.
 *
 * Unlike PSHUFB the control is a constant, so `shufflevector` takes it
 * directly. What is easy to get wrong is which lanes the permutation reaches:
 *
 *   PSHUFHW  permutes the HIGH four words and copies the low quadword
 *            through untouched.
 *   PSHUFLW  permutes the LOW four words and copies the high quadword.
 *
 * Both take two bits per lane out of the imm8, indexed from the start of the
 * half they act on -- so a control of 0x1b reverses the four words it touches
 * and leaves the other four exactly as they were. A translation that treated
 * either as a whole-register permutation would scramble the half that is
 * supposed to be copied.
 */
void Capstone2LlvmIrTranslatorX86_impl::translateSsePshufImm(cs_insn* i, cs_x86* xi, IRBuilder<>& irb)
{
	EXPECT_IS_TERNARY(i, xi, irb);
	if (xi->operands[2].type != X86_OP_IMM)
	{
		translatePseudoAsmGeneric(i, xi, irb);
		return;
	}
	uint64_t imm = static_cast<uint64_t>(xi->operands[2].imm) & 0xff;

	unsigned first = 0;
	bool vex = false;
	switch (i->id)
	{
	case X86_INS_VPSHUFHW:
		vex = true;
		// fall through
	case X86_INS_PSHUFHW:
		first = 4;
		break;
	case X86_INS_VPSHUFLW:
		vex = true;
		// fall through
	case X86_INS_PSHUFLW:
		first = 0;
		break;
	default: translatePseudoAsmGeneric(i, xi, irb); return;
	}

	if (!vex)
	{
		if (xi->operands[0].size != 16)
		{
			translatePseudoAsmGeneric(i, xi, irb);
			return;
		}
		storeOp(xi->operands[0], pshufhwlwI128(loadOp(xi->operands[1], irb), imm, first, irb), irb, eOpConv::NOTHING);
		return;
	}

	unsigned destBits = xi->operands[0].size * 8;
	if (destBits != 128 && destBits != 256)
	{
		translatePseudoAsmGeneric(i, xi, irb);
		return;
	}
	Value* src = loadVectorOp(xi->operands[1], irb, destBits);
	if (src == nullptr)
	{
		translatePseudoAsmGeneric(i, xi, irb);
		return;
	}
	Value* result = destBits == 128
		? pshufhwlwI128(src, imm, first, irb)
		: concatLane128(
			pshufhwlwI128(extractLane128(src, 0, irb), imm, first, irb),
			pshufhwlwI128(extractLane128(src, 1, irb), imm, first, irb),
			irb);
	storeVectorOp(xi->operands[0], result, destBits, irb);
}

/**
 * PBLENDW, BLENDPS, BLENDPD -- pick each lane from one operand or the other
 * according to a bit of an immediate.
 *
 * A set bit selects the SECOND operand. The lane width is what separates the
 * three: eight words, four dwords, two quadwords, all reading their bits from
 * the bottom of the same imm8.
 *
 * BLENDPS and BLENDPD are floating-point only in name -- nothing is
 * interpreted, so they are the same operation at a different width and share
 * this.
 */
void Capstone2LlvmIrTranslatorX86_impl::translateSseBlendImm(cs_insn* i, cs_x86* xi, IRBuilder<>& irb)
{
	bool vex = (i->id == X86_INS_VBLENDPS || i->id == X86_INS_VBLENDPD
		|| i->id == X86_INS_VPBLENDW || i->id == X86_INS_VPBLENDD);
	unsigned srcA = 0;
	unsigned srcB = 1;
	unsigned immOp = 2;
	if (vex)
	{
		// Capstone: vblendps ymm0, ymm1, ymm2, 5 is dest, src1, src2, imm.
		if (xi->op_count != 4)
		{
			translatePseudoAsmGeneric(i, xi, irb);
			return;
		}
		srcA = 1;
		srcB = 2;
		immOp = 3;
	}
	else
	{
		EXPECT_IS_TERNARY(i, xi, irb);
	}
	if (xi->operands[immOp].type != X86_OP_IMM)
	{
		translatePseudoAsmGeneric(i, xi, irb);
		return;
	}

	unsigned destBits = xi->operands[0].size * 8;
	if (destBits != 128 && destBits != 256)
	{
		translatePseudoAsmGeneric(i, xi, irb);
		return;
	}
	if (!vex && destBits != 128)
	{
		translatePseudoAsmGeneric(i, xi, irb);
		return;
	}

	uint64_t imm = static_cast<uint64_t>(xi->operands[immOp].imm) & 0xff;

	unsigned elemBits = 0;
	switch (i->id)
	{
	case X86_INS_PBLENDW:
	case X86_INS_VPBLENDW: elemBits = 16; break;
	case X86_INS_VPBLENDD:
	case X86_INS_BLENDPS:
	case X86_INS_VBLENDPS: elemBits = 32; break;
	case X86_INS_BLENDPD:
	case X86_INS_VBLENDPD: elemBits = 64; break;
	default: translatePseudoAsmGeneric(i, xi, irb); return;
	}
	unsigned n = destBits / elemBits;

	Value* aRaw = nullptr;
	Value* bRaw = nullptr;
	if (vex)
	{
		unsigned bits = destBits;
		aRaw = loadVectorOp(xi->operands[srcA], irb, bits);
		bits = destBits;
		bRaw = loadVectorOp(xi->operands[srcB], irb, bits);
		if (aRaw == nullptr || bRaw == nullptr)
		{
			translatePseudoAsmGeneric(i, xi, irb);
			return;
		}
	}
	else
	{
		// Three operands, so the two sources are loaded by hand: loadOpBinary()
		// refuses an instruction with an immediate on the end.
		op0 = loadOp(xi->operands[0], irb);
		op1 = loadOp(xi->operands[1], irb);
		aRaw = toI128(op0, irb);
		bRaw = toI128(op1, irb);
	}

	auto* intTy = irb.getIntNTy(destBits);
	auto* vecTy = vecType(irb.getIntNTy(elemBits), n);
	Value* a = irb.CreateBitCast(irb.CreateZExtOrTrunc(aRaw, intTy), vecTy);
	Value* b = irb.CreateBitCast(irb.CreateZExtOrTrunc(bRaw, intTy), vecTy);

	llvm::SmallVector<int, 8> mask;
	for (unsigned lane = 0; lane < n; ++lane)
	{
		bool fromSecond = ((imm >> lane) & 1) != 0;
		mask.push_back(static_cast<int>(fromSecond ? n + lane : lane));
	}
	Value* res = irb.CreateShuffleVector(a, b, mask);
	Value* asInt = irb.CreateBitCast(res, intTy);

	if (vex)
	{
		storeVectorOp(xi->operands[0], asInt, destBits, irb);
	}
	else
	{
		storeOp(xi->operands[0], asInt, irb, eOpConv::NOTHING);
	}
}

/**
 * PINSRB/W/D/Q -- insert the low bits of a GPR or memory operand into one
 * XMM (or MM, for PINSRW) lane selected by imm8. Every other lane is copied
 * through from the destination.
 *
 * The unused high bits of a 32-bit source are not part of the element:
 * `pinsrb` reads AL, not EAX. The imm8 is masked to the lane count; the
 * architecture ignores the rest of the byte.
 */
void Capstone2LlvmIrTranslatorX86_impl::translateSsePinsr(cs_insn* i, cs_x86* xi, IRBuilder<>& irb)
{
	// SSE PINSR is dest/src, gpr, imm. VEX VPINSR is dest, xmm src, gpr, imm
	// -- Capstone reports four operands, confirmed by dumping `vpinsrb xmm0,
	// xmm1, eax, 3`. Treating the VEX form as ternary would insert into dest
	// and ignore xmm1, which is the whole reason gcc emits VEX.
	unsigned vecOp = 0;
	unsigned srcOp = 1;
	unsigned immOp = 2;
	if (xi->op_count == 4)
	{
		vecOp = 1;
		srcOp = 2;
		immOp = 3;
	}
	else if (xi->op_count != 3)
	{
		translatePseudoAsmGeneric(i, xi, irb);
		return;
	}
	if (xi->operands[immOp].type != X86_OP_IMM)
	{
		translatePseudoAsmGeneric(i, xi, irb);
		return;
	}

	unsigned elemBits = 0;
	switch (i->id)
	{
	case X86_INS_PINSRB:
	case X86_INS_VPINSRB: elemBits = 8; break;
	case X86_INS_PINSRW:
	case X86_INS_VPINSRW: elemBits = 16; break;
	case X86_INS_PINSRD:
	case X86_INS_VPINSRD: elemBits = 32; break;
	case X86_INS_PINSRQ:
	case X86_INS_VPINSRQ: elemBits = 64; break;
	default: translatePseudoAsmGeneric(i, xi, irb); return;
	}

	unsigned totalBits = xi->operands[0].size * 8;
	if (totalBits < elemBits || totalBits % elemBits != 0)
	{
		translatePseudoAsmGeneric(i, xi, irb);
		return;
	}
	unsigned n = totalBits / elemBits;
	unsigned lane = static_cast<unsigned>(xi->operands[immOp].imm) & (n - 1);

	auto* intTy = irb.getIntNTy(totalBits);
	auto* vecTy = vecType(irb.getIntNTy(elemBits), n);
	Value* dst = irb.CreateBitCast(irb.CreateZExtOrTrunc(loadOp(xi->operands[vecOp], irb), intTy), vecTy);
	Value* src = irb.CreateZExtOrTrunc(loadOp(xi->operands[srcOp], irb), irb.getIntNTy(elemBits));
	Value* res = irb.CreateInsertElement(dst, src, (uint64_t)lane);

	Value* asInt = irb.CreateBitCast(res, intTy);
	if (xi->op_count == 4)
	{
		// VEX 128-bit write zeroes YMM/ZMM above the destination. SSE PINSR
		// leaves those bits alone, which is why the two encodings exist.
		storeVectorOp(xi->operands[0], asInt, totalBits, irb);
	}
	else
	{
		storeOp(xi->operands[0], asInt, irb, eOpConv::NOTHING);
	}
}

/**
 * PEXTRB/W/D/Q, VPEXTRB/W/D/Q and EXTRACTPS -- copy one lane of an XMM (or
 * MM, for PEXTRW) to a GPR or to memory. EXTRACTPS is PEXTRD on the same
 * bits: the float interpretation is only the encoding.
 *
 * VPEXTR is the same three operands as PEXTR (dest, xmm, imm) -- unlike
 * VPINSR there is no extra VEX source, because the destination is a GPR.
 * PEXTRW's destination is historically r32, zero-extended from the word.
 * The same zero-extend applies to PEXTRB. Memory destinations keep the
 * element's own width.
 */
void Capstone2LlvmIrTranslatorX86_impl::translateSsePextr(cs_insn* i, cs_x86* xi, IRBuilder<>& irb)
{
	EXPECT_IS_TERNARY(i, xi, irb);
	if (xi->operands[2].type != X86_OP_IMM)
	{
		translatePseudoAsmGeneric(i, xi, irb);
		return;
	}

	unsigned elemBits = 0;
	switch (i->id)
	{
	case X86_INS_PEXTRB:
	case X86_INS_VPEXTRB: elemBits = 8; break;
	case X86_INS_PEXTRW:
	case X86_INS_VPEXTRW: elemBits = 16; break;
	case X86_INS_PEXTRD:
	case X86_INS_VPEXTRD:
	case X86_INS_EXTRACTPS:
	case X86_INS_VEXTRACTPS: elemBits = 32; break;
	case X86_INS_PEXTRQ:
	case X86_INS_VPEXTRQ: elemBits = 64; break;
	default: translatePseudoAsmGeneric(i, xi, irb); return;
	}

	unsigned totalBits = xi->operands[1].size * 8;
	if (totalBits < elemBits || totalBits % elemBits != 0)
	{
		translatePseudoAsmGeneric(i, xi, irb);
		return;
	}
	unsigned n = totalBits / elemBits;
	unsigned lane = static_cast<unsigned>(xi->operands[2].imm) & (n - 1);

	auto* intTy = irb.getIntNTy(totalBits);
	auto* vecTy = vecType(irb.getIntNTy(elemBits), n);
	Value* src = irb.CreateBitCast(irb.CreateZExtOrTrunc(loadOp(xi->operands[1], irb), intTy), vecTy);
	Value* elem = irb.CreateExtractElement(src, (uint64_t)lane);

	// GPR destinations are at least 32 bits (PEXTRW's r32 dest). Memory
	// keeps the lane width; ZEXT_TRUNC_OR_BITCAST cuts it back.
	unsigned outBits = elemBits < 32 ? 32 : elemBits;
	storeOp(
		xi->operands[0],
		irb.CreateZExt(elem, irb.getIntNTy(outBits)),
		irb,
		eOpConv::ZEXT_TRUNC_OR_BITCAST);
}

/**
 * INSERTPS dest, src, imm8 -- insert one f32 lane and then zero lanes
 * selected by a nibble.
 *
 *   imm[7:6]  COUNT_S   source lane (ignored when src is memory)
 *   imm[5:4]  COUNT_D   destination lane
 *   imm[3:0]  ZMASK     one bit per f32 lane, set means write zero
 *
 * The insert happens first, so a lane that is both the destination and in
 * the zero mask ends up zero. That is the SDM order, not a choice.
 */
void Capstone2LlvmIrTranslatorX86_impl::translateSseInsertps(cs_insn* i, cs_x86* xi, IRBuilder<>& irb)
{
	bool vex = (i->id == X86_INS_VINSERTPS);
	if (vex)
	{
		if (xi->op_count != 4 || xi->operands[3].type != X86_OP_IMM || xi->operands[0].size != 16)
		{
			translatePseudoAsmGeneric(i, xi, irb);
			return;
		}
	}
	else
	{
		EXPECT_IS_TERNARY(i, xi, irb);
		if (xi->operands[2].type != X86_OP_IMM || xi->operands[0].size != 16)
		{
			translatePseudoAsmGeneric(i, xi, irb);
			return;
		}
	}

	unsigned immOp = vex ? 3 : 2;
	unsigned srcOp = vex ? 2 : 1;
	unsigned dstOp = vex ? 1 : 0;
	unsigned imm = static_cast<unsigned>(xi->operands[immOp].imm) & 0xff;
	unsigned countS = (imm >> 6) & 3;
	unsigned countD = (imm >> 4) & 3;
	unsigned zmask = imm & 0xf;
	if (xi->operands[srcOp].type == X86_OP_MEM)
	{
		countS = 0;
	}

	auto* vec4f = vecType(irb.getFloatTy(), 4);
	Value* dst = irb.CreateBitCast(toI128(loadOp(xi->operands[dstOp], irb), irb), vec4f);
	Value* src = irb.CreateBitCast(toI128(loadOp(xi->operands[srcOp], irb), irb), vec4f);
	Value* elem = irb.CreateExtractElement(src, (uint64_t)countS);
	Value* res = irb.CreateInsertElement(dst, elem, (uint64_t)countD);

	Value* zero = ConstantFP::get(irb.getFloatTy(), 0.0);
	for (unsigned lane = 0; lane < 4; ++lane)
	{
		if ((zmask >> lane) & 1)
		{
			res = irb.CreateInsertElement(res, zero, (uint64_t)lane);
		}
	}

	if (vex)
	{
		storeVectorOp(xi->operands[0], irb.CreateBitCast(res, irb.getInt128Ty()), 128, irb);
	}
	else
	{
		storeOp(xi->operands[0], irb.CreateBitCast(res, irb.getInt128Ty()), irb, eOpConv::NOTHING);
	}
}

/**
 * PBLENDVB, BLENDVPS, BLENDVPD -- blend using the MSB of each lane of an
 * implicit XMM0 mask (per byte, per f32, per f64). A set MSB selects the
 * SECOND operand, matching PBLENDW's immediate bit.
 *
 * Capstone may list XMM0 as a third operand or hide it. Two and three
 * operands are both the SSE4.1 form. Four operands is AVX VPBLENDVB and
 * is left as pseudo-assembly -- do not invent a 4th operand here.
 */
void Capstone2LlvmIrTranslatorX86_impl::translateSseBlendV(cs_insn* i, cs_x86* xi, IRBuilder<>& irb)
{
	unsigned elemBits = 0;
	bool vex = false;
	switch (i->id)
	{
	case X86_INS_VPBLENDVB:
		vex = true;
		// fall through
	case X86_INS_PBLENDVB: elemBits = 8; break;
	case X86_INS_VBLENDVPS:
		vex = true;
		// fall through
	case X86_INS_BLENDVPS: elemBits = 32; break;
	case X86_INS_VBLENDVPD:
		vex = true;
		// fall through
	case X86_INS_BLENDVPD: elemBits = 64; break;
	default: translatePseudoAsmGeneric(i, xi, irb); return;
	}

	unsigned destBits = vex ? (xi->operands[0].size * 8) : 128;
	if ((vex && destBits != 128 && destBits != 256) || destBits % elemBits != 0)
	{
		translatePseudoAsmGeneric(i, xi, irb);
		return;
	}
	unsigned n = destBits / elemBits;
	Value* aRaw = nullptr;
	Value* bRaw = nullptr;
	Value* mRaw = nullptr;
	if (vex)
	{
		// Capstone: vblendvps xmm0, xmm1, xmm2, xmm3 is dest, src1, src2, mask.
		if (xi->op_count != 4)
		{
			translatePseudoAsmGeneric(i, xi, irb);
			return;
		}
		aRaw = loadVectorOp(xi->operands[1], irb, destBits);
		bRaw = loadVectorOp(xi->operands[2], irb, destBits);
		mRaw = loadVectorOp(xi->operands[3], irb, destBits);
		if (aRaw == nullptr || bRaw == nullptr || mRaw == nullptr)
		{
			translatePseudoAsmGeneric(i, xi, irb);
			return;
		}
	}
	else
	{
		EXPECT_IS_BINARY_OR_TERNARY(i, xi, irb);
		if (xi->operands[0].size != 16)
		{
			translatePseudoAsmGeneric(i, xi, irb);
			return;
		}
		aRaw = toI128(loadOp(xi->operands[0], irb), irb);
		bRaw = toI128(loadOp(xi->operands[1], irb), irb);
		mRaw = (xi->op_count == 3 && xi->operands[2].type == X86_OP_REG)
			? toI128(loadOp(xi->operands[2], irb), irb)
			: toI128(loadRegister(X86_REG_XMM0, irb), irb);
	}

	auto* intTy = irb.getIntNTy(destBits);
	auto* vecTy = vecType(irb.getIntNTy(elemBits), n);
	Value* a = irb.CreateBitCast(irb.CreateZExtOrTrunc(aRaw, intTy), vecTy);
	Value* b = irb.CreateBitCast(irb.CreateZExtOrTrunc(bRaw, intTy), vecTy);
	Value* m = irb.CreateBitCast(irb.CreateZExtOrTrunc(mRaw, intTy), vecTy);
	Value* res = irb.CreateSelect(irb.CreateICmpSLT(m, Constant::getNullValue(vecTy)), b, a);
	Value* asInt = irb.CreateBitCast(res, intTy);
	if (vex)
	{
		storeVectorOp(xi->operands[0], asInt, destBits, irb);
	}
	else
	{
		storeOp(xi->operands[0], asInt, irb, eOpConv::NOTHING);
	}
}

/**
 * PTEST -- logical compare of two XMM registers into ZF and CF, destination
 * unmodified.
 *
 *   ZF = (xmm1 AND xmm2) == 0
 *   CF = (xmm1 ANDNOT xmm2) == 0   i.e. ((NOT xmm1) AND xmm2) == 0
 *
 * OF, SF, AF, PF are cleared, the same shape KTEST uses.
 */
void Capstone2LlvmIrTranslatorX86_impl::translateSsePtest(cs_insn* i, cs_x86* xi, IRBuilder<>& irb)
{
	EXPECT_IS_BINARY(i, xi, irb);

	unsigned destBits = xi->operands[0].size * 8;
	if (destBits != 128 && destBits != 256)
	{
		destBits = 128;
	}
	unsigned bits = destBits;
	Value* a = loadVectorOp(xi->operands[0], irb, bits);
	bits = destBits;
	Value* b = loadVectorOp(xi->operands[1], irb, bits);
	if (a == nullptr || b == nullptr)
	{
		a = toI128(loadOp(xi->operands[0], irb), irb);
		b = toI128(loadOp(xi->operands[1], irb), irb);
		destBits = 128;
	}
	auto* intTy = irb.getIntNTy(destBits);
	a = irb.CreateZExtOrTrunc(a, intTy);
	b = irb.CreateZExtOrTrunc(b, intTy);

	unsigned msbBits = 0;
	if (i->id == X86_INS_VTESTPS)
	{
		msbBits = 32;
	}
	else if (i->id == X86_INS_VTESTPD)
	{
		msbBits = 64;
	}
	if (msbBits != 0)
	{
		unsigned n = destBits / msbBits;
		auto* vecTy = vecType(irb.getIntNTy(msbBits), n);
		Value* va = irb.CreateBitCast(a, vecTy);
		Value* vb = irb.CreateBitCast(b, vecTy);
		Value* zero = Constant::getNullValue(vecTy);
		Value* signAnd = irb.CreateICmpSLT(irb.CreateAnd(va, vb), zero);
		Value* signAndn = irb.CreateICmpSLT(irb.CreateAnd(irb.CreateNot(va), vb), zero);
		Value* anyAnd = irb.getFalse();
		Value* anyAndn = irb.getFalse();
		for (unsigned lane = 0; lane < n; ++lane)
		{
			anyAnd = irb.CreateOr(anyAnd, irb.CreateExtractElement(signAnd, lane));
			anyAndn = irb.CreateOr(anyAndn, irb.CreateExtractElement(signAndn, lane));
		}
		storeMaskTestFlags(irb.CreateNot(anyAnd), irb.CreateNot(anyAndn), irb);
		return;
	}

	Value* zero = ConstantInt::get(intTy, 0);
	Value* zf = irb.CreateICmpEQ(irb.CreateAnd(a, b), zero);
	Value* cf = irb.CreateICmpEQ(irb.CreateAnd(irb.CreateNot(a), b), zero);
	storeMaskTestFlags(zf, cf, irb);
}

/**
 * PSLLW/D/Q, PSRLW/D/Q, PSRAW/D -- the packed shifts, in both the
 * register/memory form and the immediate one. Capstone gives both the same
 * instruction id and distinguishes them by the operand's type.
 *
 * Two things separate this from a vector shl.
 *
 * The count is ONE value for the whole register -- the low quadword of the
 * second operand, or the immediate -- not one per lane. The upper quadword of
 * a register count operand is not read at all.
 *
 * A count at or past the element width is DEFINED, and it is defined
 * differently for the two directions: a logical shift gives zero, an
 * arithmetic right shift gives the sign bit broadcast across the element.
 * `psllw` by 17 is zero, measured. LLVM's shl is poison there, so the count
 * is clamped and the logical case is selected back to zero -- the same shape
 * as the conversion guard a few hundred lines up, and for the same reason:
 * poison does not stay where it is put.
 */
static Value* packedShiftI128(
	Value* src, Value* cntI64, unsigned elemBits, bool left, bool arithmetic, IRBuilder<>& irb)
{
	unsigned n = 128 / elemBits;
	auto* vecTy = vecType(irb.getIntNTy(elemBits), n);
	Value* v = irb.CreateBitCast(toI128(src, irb), vecTy);
	auto* i64 = irb.getInt64Ty();
	auto* tooBig = irb.CreateICmpUGE(cntI64, ConstantInt::get(i64, elemBits));
	auto* safe = irb.CreateSelect(tooBig, ConstantInt::get(i64, elemBits - 1), cntI64);
	Value* amt = irb.CreateVectorSplat(n, irb.CreateTrunc(safe, irb.getIntNTy(elemBits)));
	Value* res = nullptr;
	if (left)
	{
		res = irb.CreateShl(v, amt);
	}
	else if (arithmetic)
	{
		res = irb.CreateAShr(v, amt);
	}
	else
	{
		res = irb.CreateLShr(v, amt);
	}
	if (!arithmetic)
	{
		res = irb.CreateSelect(tooBig, Constant::getNullValue(vecTy), res);
	}
	return irb.CreateBitCast(res, irb.getInt128Ty());
}

void Capstone2LlvmIrTranslatorX86_impl::translateSsePackedShift(cs_insn* i, cs_x86* xi, IRBuilder<>& irb)
{
	unsigned elemBits = 0;
	bool left = false;
	bool arithmetic = false;
	bool vex = false;
	switch (i->id)
	{
	case X86_INS_VPSLLW:
		vex = true;
		// fall through
	case X86_INS_PSLLW:
		elemBits = 16;
		left = true;
		break;
	case X86_INS_VPSLLD:
		vex = true;
		// fall through
	case X86_INS_PSLLD:
		elemBits = 32;
		left = true;
		break;
	case X86_INS_VPSLLQ:
		vex = true;
		// fall through
	case X86_INS_PSLLQ:
		elemBits = 64;
		left = true;
		break;
	case X86_INS_VPSRLW:
		vex = true;
		// fall through
	case X86_INS_PSRLW: elemBits = 16; break;
	case X86_INS_VPSRLD:
		vex = true;
		// fall through
	case X86_INS_PSRLD: elemBits = 32; break;
	case X86_INS_VPSRLQ:
		vex = true;
		// fall through
	case X86_INS_PSRLQ: elemBits = 64; break;
	case X86_INS_VPSRAW:
		vex = true;
		// fall through
	case X86_INS_PSRAW:
		elemBits = 16;
		arithmetic = true;
		break;
	case X86_INS_VPSRAD:
		vex = true;
		// fall through
	case X86_INS_PSRAD:
		elemBits = 32;
		arithmetic = true;
		break;
	case X86_INS_VPSRAQ:
		vex = true;
		elemBits = 64;
		arithmetic = true;
		break;
	default: translatePseudoAsmGeneric(i, xi, irb); return;
	}

	if (vex)
	{
		// Capstone: vpsllw xmm0, xmm1, 4 / vpsllw ymm0, ymm1, xmm2 -- dest,
		// src, count. The count is an immediate or an XMM even when the data
		// is YMM. AVX2 shifts each 128-bit lane independently, same as VPSLLDQ.
		if (xi->op_count != 3)
		{
			translatePseudoAsmGeneric(i, xi, irb);
			return;
		}
		unsigned destBits = xi->operands[0].size * 8;
		if ((destBits != 128 && destBits != 256) || destBits % elemBits != 0)
		{
			translatePseudoAsmGeneric(i, xi, irb);
			return;
		}

		unsigned srcBits = destBits;
		Value* src = loadVectorOp(xi->operands[1], irb, srcBits);
		if (src == nullptr)
		{
			translatePseudoAsmGeneric(i, xi, irb);
			return;
		}

		auto* i64 = irb.getInt64Ty();
		Value* cnt = nullptr;
		if (xi->operands[2].type == X86_OP_IMM)
		{
			cnt = ConstantInt::get(i64, xi->operands[2].imm);
		}
		else
		{
			unsigned cntBits = xi->operands[2].size * 8;
			if (cntBits == 0)
			{
				cntBits = 128;
			}
			Value* c = loadVectorOp(xi->operands[2], irb, cntBits);
			if (c == nullptr)
			{
				translatePseudoAsmGeneric(i, xi, irb);
				return;
			}
			cnt = irb.CreateTrunc(irb.CreateZExtOrTrunc(c, irb.getInt128Ty()), i64);
		}

		Value* result = nullptr;
		if (destBits == 128)
		{
			result = packedShiftI128(src, cnt, elemBits, left, arithmetic, irb);
		}
		else
		{
			result = concatLane128(
				packedShiftI128(extractLane128(src, 0, irb), cnt, elemBits, left, arithmetic, irb),
				packedShiftI128(extractLane128(src, 1, irb), cnt, elemBits, left, arithmetic, irb),
				irb);
		}
		storeVectorOp(xi->operands[0], result, destBits, irb);
		return;
	}

	EXPECT_IS_BINARY(i, xi, irb);

	// MMX shares these mnemonics and these ids at half the width. Deciding
	// from the operand rather than assuming 128 bits is what keeps the two
	// apart -- the id alone does not say.
	unsigned totalBits = xi->operands[0].size * 8;
	if (totalBits < elemBits || totalBits % elemBits != 0)
	{
		translatePseudoAsmGeneric(i, xi, irb);
		return;
	}
	unsigned n = totalBits / elemBits;

	std::tie(op0, op1) = loadOpBinary(xi, irb, eOpConv::NOTHING);

	auto* intTy = irb.getIntNTy(totalBits);
	auto* vecTy = vecType(irb.getIntNTy(elemBits), n);
	Value* v = irb.CreateBitCast(irb.CreateZExtOrTrunc(toI128(op0, irb), intTy), vecTy);

	auto* i64 = irb.getInt64Ty();
	Value* cnt = nullptr;
	if (xi->operands[1].type == X86_OP_IMM)
	{
		cnt = ConstantInt::get(i64, xi->operands[1].imm);
	}
	else
	{
		// The low quadword only: the rest of the register is not the count.
		cnt = irb.CreateTrunc(toI128(op1, irb), i64);
	}

	auto* tooBig = irb.CreateICmpUGE(cnt, ConstantInt::get(i64, elemBits));
	// Clamped to the largest count that is not poison. For the arithmetic
	// shift that clamp IS the answer -- shifting right by width-1 leaves the
	// sign bit everywhere -- so only the logical cases need the select below.
	auto* safe = irb.CreateSelect(tooBig, ConstantInt::get(i64, elemBits - 1), cnt);
	Value* amt = irb.CreateVectorSplat(n, irb.CreateTrunc(safe, irb.getIntNTy(elemBits)));

	Value* res = nullptr;
	if (left)
	{
		res = irb.CreateShl(v, amt);
	}
	else if (arithmetic)
	{
		res = irb.CreateAShr(v, amt);
	}
	else
	{
		res = irb.CreateLShr(v, amt);
	}
	if (!arithmetic)
	{
		res = irb.CreateSelect(tooBig, Constant::getNullValue(vecTy), res);
	}

	storeOp(
		xi->operands[0],
		irb.CreateZExtOrTrunc(irb.CreateBitCast(res, intTy), irb.getInt128Ty()),
		irb,
		eOpConv::NOTHING);
}

/**
 * PMOVSXBW/BD/BQ/WD/WQ/DQ and the PMOVZX forms -- widen the LOW elements of
 * the source and fill the destination with them.
 *
 * How many elements are read follows from the destination, not the source:
 * `pmovsxbq` writes two quadwords, so it reads two bytes. Everything above
 * the low sixteen bits of the source is untouched input, and a translator
 * that reads more than it should would still produce a plausible answer for
 * the lanes it does write -- which is why the oracle fills those bits with
 * noise.
 */
void Capstone2LlvmIrTranslatorX86_impl::translateSseMovWiden(cs_insn* i, cs_x86* xi, IRBuilder<>& irb)
{
	EXPECT_IS_BINARY(i, xi, irb);

	unsigned srcBits = 0;
	unsigned dstBits = 0;
	bool isSigned = true;
	bool vex = false;
	switch (i->id)
	{
	case X86_INS_VPMOVSXBW:
		vex = true;
		// fall through
	case X86_INS_PMOVSXBW:
		srcBits = 8;
		dstBits = 16;
		break;
	case X86_INS_VPMOVSXBD:
		vex = true;
		// fall through
	case X86_INS_PMOVSXBD:
		srcBits = 8;
		dstBits = 32;
		break;
	case X86_INS_VPMOVSXBQ:
		vex = true;
		// fall through
	case X86_INS_PMOVSXBQ:
		srcBits = 8;
		dstBits = 64;
		break;
	case X86_INS_VPMOVSXWD:
		vex = true;
		// fall through
	case X86_INS_PMOVSXWD:
		srcBits = 16;
		dstBits = 32;
		break;
	case X86_INS_VPMOVSXWQ:
		vex = true;
		// fall through
	case X86_INS_PMOVSXWQ:
		srcBits = 16;
		dstBits = 64;
		break;
	case X86_INS_VPMOVSXDQ:
		vex = true;
		// fall through
	case X86_INS_PMOVSXDQ:
		srcBits = 32;
		dstBits = 64;
		break;
	case X86_INS_VPMOVZXBW:
		vex = true;
		// fall through
	case X86_INS_PMOVZXBW:
		srcBits = 8;
		dstBits = 16;
		isSigned = false;
		break;
	case X86_INS_VPMOVZXBD:
		vex = true;
		// fall through
	case X86_INS_PMOVZXBD:
		srcBits = 8;
		dstBits = 32;
		isSigned = false;
		break;
	case X86_INS_VPMOVZXBQ:
		vex = true;
		// fall through
	case X86_INS_PMOVZXBQ:
		srcBits = 8;
		dstBits = 64;
		isSigned = false;
		break;
	case X86_INS_VPMOVZXWD:
		vex = true;
		// fall through
	case X86_INS_PMOVZXWD:
		srcBits = 16;
		dstBits = 32;
		isSigned = false;
		break;
	case X86_INS_VPMOVZXWQ:
		vex = true;
		// fall through
	case X86_INS_PMOVZXWQ:
		srcBits = 16;
		dstBits = 64;
		isSigned = false;
		break;
	case X86_INS_VPMOVZXDQ:
		vex = true;
		// fall through
	case X86_INS_PMOVZXDQ:
		srcBits = 32;
		dstBits = 64;
		isSigned = false;
		break;
	default: translatePseudoAsmGeneric(i, xi, irb); return;
	}

	unsigned destBits = vex ? (xi->operands[0].size * 8) : 128;
	if ((vex && destBits != 128 && destBits != 256) || destBits % dstBits != 0)
	{
		translatePseudoAsmGeneric(i, xi, irb);
		return;
	}
	unsigned n = destBits / dstBits;
	EXPECT_IS_BINARY(i, xi, irb);
	op1 = loadOpBinaryOp1(xi, irb);
	if (op1 == nullptr)
	{
		translatePseudoAsmGeneric(i, xi, irb);
		return;
	}

	auto* srcVecTy = vecType(irb.getIntNTy(srcBits), 128 / srcBits);
	Value* sv = irb.CreateBitCast(toI128(op1, irb), srcVecTy);

	llvm::SmallVector<int, 16> mask;
	for (unsigned k = 0; k < n; ++k)
	{
		mask.push_back(static_cast<int>(k));
	}
	Value* low = irb.CreateShuffleVector(sv, llvm::UndefValue::get(srcVecTy), mask);

	auto* dstVecTy = vecType(irb.getIntNTy(dstBits), n);
	Value* ext = isSigned ? irb.CreateSExt(low, dstVecTy) : irb.CreateZExt(low, dstVecTy);
	Value* asInt = irb.CreateBitCast(ext, irb.getIntNTy(destBits));
	if (vex)
	{
		storeVectorOp(xi->operands[0], asInt, destBits, irb);
	}
	else
	{
		storeOp(xi->operands[0], asInt, irb, eOpConv::NOTHING);
	}
}

void Capstone2LlvmIrTranslatorX86_impl::translateSsePminMax(cs_insn* i, cs_x86* xi, IRBuilder<>& irb)
{
	EXPECT_IS_BINARY(i, xi, irb);
	std::tie(op0, op1) = loadOpBinary(xi, irb, eOpConv::NOTHING);

	unsigned bits = 0;
	bool isSigned = false;
	bool isMax = false;
	switch (i->id)
	{
	case X86_INS_PMINUB: bits = 8; break;
	case X86_INS_PMINUW: bits = 16; break;
	case X86_INS_PMINUD: bits = 32; break;
	case X86_INS_PMAXUB:
		bits = 8;
		isMax = true;
		break;
	case X86_INS_PMAXUW:
		bits = 16;
		isMax = true;
		break;
	case X86_INS_PMAXUD:
		bits = 32;
		isMax = true;
		break;
	case X86_INS_PMINSB:
		bits = 8;
		isSigned = true;
		break;
	case X86_INS_PMINSW:
		bits = 16;
		isSigned = true;
		break;
	case X86_INS_PMINSD:
		bits = 32;
		isSigned = true;
		break;
	case X86_INS_PMAXSB:
		bits = 8;
		isSigned = true;
		isMax = true;
		break;
	case X86_INS_PMAXSW:
		bits = 16;
		isSigned = true;
		isMax = true;
		break;
	case X86_INS_PMAXSD:
		bits = 32;
		isSigned = true;
		isMax = true;
		break;
	default: throw GenericError("translateSsePminMax(): unhandled instruction id");
	}
	unsigned lanes = 128 / bits;
	auto* vecTy = FixedVectorType::get(irb.getIntNTy(bits), lanes);
	Value* v0 = irb.CreateBitCast(toI128(op0, irb), vecTy);
	Value* v1 = irb.CreateBitCast(toI128(op1, irb), vecTy);

	Value* cmp = nullptr;
	if (isMax)
	{
		cmp = isSigned ? irb.CreateICmpSGT(v0, v1) : irb.CreateICmpUGT(v0, v1);
	}
	else
	{
		cmp = isSigned ? irb.CreateICmpSLT(v0, v1) : irb.CreateICmpULT(v0, v1);
	}
	Value* res = irb.CreateSelect(cmp, v0, v1);
	storeOp(xi->operands[0], irb.CreateBitCast(res, irb.getInt128Ty()), irb, eOpConv::NOTHING);
}

/**
 * PALIGNR dst, src, imm — concatenate dst:src and take 16 bytes starting
 * `imm` bytes in from the bottom.
 *
 *     temp = (dst << 128) | src      (256 bits, dst on top)
 *     dst  = temp >> (imm * 8)       (low 128 bits of that)
 *
 * Done on i128 rather than i256 on purpose. The shift amount is a compile-time
 * constant here, so the three cases can be separated in C++ and every LLVM
 * shift kept strictly below its operand's width -- a shift of exactly the
 * width is poison, and `imm == 0` and `imm == 16` are the two values that
 * would produce one. The i256 spelling reads better and puts an
 * arbitrary-precision shift through tests/llvmir-emul for no gain.
 *
 * `imm >= 32` is architecturally zero, not poison and not a wrap: the window
 * has moved entirely past the top of the concatenation.
 */
/// PALIGNR of one 128-bit pair: the result is the 16-byte window starting
/// `n` bytes into the concatenation of hi:lo. n of 32 or more is zero.
static Value* palignrI128(Value* hi, Value* lo, unsigned n, IRBuilder<>& irb)
{
	auto* i128 = irb.getInt128Ty();
	hi = toI128(hi, irb);
	lo = toI128(lo, irb);
	if (n == 0)
	{
		return lo;
	}
	if (n < 16)
	{
		Value* a = irb.CreateLShr(lo, ConstantInt::get(i128, n * 8));
		Value* b = irb.CreateShl(hi, ConstantInt::get(i128, 128 - n * 8));
		return irb.CreateOr(b, a);
	}
	if (n < 32)
	{
		return irb.CreateLShr(hi, ConstantInt::get(i128, (n - 16) * 8));
	}
	return ConstantInt::get(i128, 0);
}

void Capstone2LlvmIrTranslatorX86_impl::translateSsePalignr(cs_insn* i, cs_x86* xi, IRBuilder<>& irb)
{
	bool vex = (i->id == X86_INS_VPALIGNR);
	unsigned hiOp = 0;
	unsigned loOp = 1;
	unsigned immOp = 2;
	if (vex)
	{
		// Capstone: vpalignr xmm0, xmm1, xmm2, 4 is dest, src1, src2, imm.
		if (xi->op_count != 4 || xi->operands[3].type != X86_OP_IMM)
		{
			translatePseudoAsmGeneric(i, xi, irb);
			return;
		}
		hiOp = 1;
		loOp = 2;
		immOp = 3;
	}
	else
	{
		EXPECT_IS_TERNARY(i, xi, irb);
	}

	unsigned n = static_cast<unsigned>(xi->operands[immOp].imm) & 0xff;

	if (!vex)
	{
		Value* dst = toI128(loadOp(xi->operands[hiOp], irb), irb);
		Value* src = toI128(loadOp(xi->operands[loOp], irb), irb);
		storeOp(xi->operands[0], palignrI128(dst, src, n, irb), irb, eOpConv::NOTHING);
		return;
	}

	unsigned bits = xi->operands[0].size * 8;
	if (bits != 128 && bits != 256)
	{
		translatePseudoAsmGeneric(i, xi, irb);
		return;
	}

	unsigned loadBits = bits;
	Value* hi = loadVectorOp(xi->operands[hiOp], irb, loadBits);
	loadBits = bits;
	Value* lo = loadVectorOp(xi->operands[loOp], irb, loadBits);
	if (hi == nullptr || lo == nullptr)
	{
		translatePseudoAsmGeneric(i, xi, irb);
		return;
	}

	Value* res = nullptr;
	if (bits == 128)
	{
		res = palignrI128(hi, lo, n, irb);
	}
	else
	{
		// AVX2 VPALIGNR is two independent 16-byte aligns, not one 32-byte
		// window across the whole YMM.
		auto* i256 = irb.getIntNTy(256);
		hi = irb.CreateZExtOrTrunc(hi, i256);
		lo = irb.CreateZExtOrTrunc(lo, i256);
		auto* i128 = irb.getInt128Ty();
		Value* hiLo = irb.CreateTrunc(hi, i128);
		Value* hiHi = irb.CreateTrunc(irb.CreateLShr(hi, ConstantInt::get(i256, 128)), i128);
		Value* loLo = irb.CreateTrunc(lo, i128);
		Value* loHi = irb.CreateTrunc(irb.CreateLShr(lo, ConstantInt::get(i256, 128)), i128);
		Value* outLo = palignrI128(hiLo, loLo, n, irb);
		Value* outHi = palignrI128(hiHi, loHi, n, irb);
		res = irb.CreateOr(
			irb.CreateZExt(outLo, i256),
			irb.CreateShl(irb.CreateZExt(outHi, i256), ConstantInt::get(i256, 128)));
	}
	storeVectorOp(xi->operands[0], res, bits, irb);
}

/**
 * PAVGB, PAVGW — packed unsigned average with round-half-up.
 *
 *     dst[k] = (dst[k] + src[k] + 1) >> 1
 *
 * The addition is done one bit wider than the lane. At the lane's own width
 * 0xff + 0xff + 1 wraps to 0xff, which is also the right answer, so a
 * truncated version passes a test over saturating inputs and fails over
 * 0x80 + 0x80: 0x81 instead of 0x80. The widening is not a precaution, it is
 * the instruction.
 */
void Capstone2LlvmIrTranslatorX86_impl::translateSsePavg(cs_insn* i, cs_x86* xi, IRBuilder<>& irb)
{
	unsigned bits = (i->id == X86_INS_PAVGB || i->id == X86_INS_VPAVGB) ? 8 : 16;
	bool vex = (i->id == X86_INS_VPAVGB || i->id == X86_INS_VPAVGW);
	if (vex)
	{
		if (xi->op_count != 3)
		{
			translatePseudoAsmGeneric(i, xi, irb);
			return;
		}
		unsigned destBits = xi->operands[0].size * 8;
		if (destBits != 128 && destBits != 256)
		{
			translatePseudoAsmGeneric(i, xi, irb);
			return;
		}
		unsigned loadBits = destBits;
		Value* aRaw = loadVectorOp(xi->operands[1], irb, loadBits);
		loadBits = destBits;
		Value* bRaw = loadVectorOp(xi->operands[2], irb, loadBits);
		if (aRaw == nullptr || bRaw == nullptr)
		{
			translatePseudoAsmGeneric(i, xi, irb);
			return;
		}
		unsigned lanes = destBits / bits;
		auto* vecTy = FixedVectorType::get(irb.getIntNTy(bits), lanes);
		auto* wideTy = FixedVectorType::get(irb.getIntNTy(bits * 2), lanes);
		auto* intTy = irb.getIntNTy(destBits);
		Value* v0 = irb.CreateZExt(irb.CreateBitCast(irb.CreateZExtOrTrunc(aRaw, intTy), vecTy), wideTy);
		Value* v1 = irb.CreateZExt(irb.CreateBitCast(irb.CreateZExtOrTrunc(bRaw, intTy), vecTy), wideTy);
		Value* one = ConstantInt::get(wideTy, 1);
		Value* avg = irb.CreateTrunc(irb.CreateLShr(irb.CreateAdd(irb.CreateAdd(v0, v1), one), one), vecTy);
		storeVectorOp(xi->operands[0], irb.CreateBitCast(avg, intTy), destBits, irb);
		return;
	}

	EXPECT_IS_BINARY(i, xi, irb);
	std::tie(op0, op1) = loadOpBinary(xi, irb, eOpConv::NOTHING);

	unsigned lanes = 128 / bits;
	auto* vecTy = FixedVectorType::get(irb.getIntNTy(bits), lanes);
	auto* wideTy = FixedVectorType::get(irb.getIntNTy(bits * 2), lanes);

	Value* v0 = irb.CreateZExt(irb.CreateBitCast(toI128(op0, irb), vecTy), wideTy);
	Value* v1 = irb.CreateZExt(irb.CreateBitCast(toI128(op1, irb), vecTy), wideTy);
	Value* one = ConstantInt::get(wideTy, 1);
	Value* sum = irb.CreateAdd(irb.CreateAdd(v0, v1), one);
	Value* avg = irb.CreateTrunc(irb.CreateLShr(sum, one), vecTy);

	storeOp(xi->operands[0], irb.CreateBitCast(avg, irb.getInt128Ty()), irb, eOpConv::NOTHING);
}

//
//==============================================================================
// AVX: the VEX-encoded integer instructions.
//==============================================================================
//

/**
 * All thirty-two of each. Registers 16..31 are EVEX-only -- no legacy SSE or
 * VEX instruction can name them -- but they are held and decomposed exactly
 * like the first sixteen, because a uniform rule is one rule. Stopping the
 * range at 15, which is what the AVX2-era version of these predicates did,
 * sends every `vmovdqu64 (%rdi),%ymm17` to pseudo-assembly.
 */
static bool isZmmRegister(uint32_t r)
{
	return X86_REG_ZMM0 <= r && r <= X86_REG_ZMM31;
}

static bool isYmmRegister(uint32_t r)
{
	return X86_REG_YMM0 <= r && r <= X86_REG_YMM31;
}

static bool isXmmRegister(uint32_t r)
{
	return X86_REG_XMM0 <= r && r <= X86_REG_XMM31;
}

/**
 * The index 0..31 of whichever vector register an operand names, whatever
 * width it was written at. zmm3, ymm3 and xmm3 are three names for three
 * nested slices of one register, so they share a number.
 */
static unsigned vectorRegisterIndex(uint32_t r)
{
	if (isZmmRegister(r))
	{
		return r - X86_REG_ZMM0;
	}
	if (isYmmRegister(r))
	{
		return r - X86_REG_YMM0;
	}
	return r - X86_REG_XMM0;
}

/**
 * The three registers a vector operand is made of: ZMM = ZMMH:YMMH:XMM.
 *
 * The low 128 bits live in the XMM global, which is what makes an SSE write
 * and an AVX or EVEX read of the same register see each other. The separate
 * i256 YMM and i512 ZMM globals in the register file are not used by any of
 * this -- using them would reintroduce exactly the aliasing bug the
 * decomposition exists to avoid.
 */
static uint32_t vectorLow128(uint32_t r)
{
	return X86_REG_XMM0 + vectorRegisterIndex(r);
}

static uint32_t vectorMid128(uint32_t r)
{
	return X86_REG_YMM0_HI + vectorRegisterIndex(r);
}

static uint32_t vectorHigh256(uint32_t r)
{
	return X86_REG_ZMM0_HI + vectorRegisterIndex(r);
}

/**
 * A vector operand read at its full width -- 128 bits for an XMM or memory
 * operand, 256 for a YMM one.
 *
 * Returns nullptr for anything this cannot express, which is what sends the
 * caller to the pseudo-assembly path rather than to a wrong answer at the
 * wrong width.
 */
llvm::Value* Capstone2LlvmIrTranslatorX86_impl::loadVectorOp(cs_x86_op& op, llvm::IRBuilder<>& irb, unsigned& bits)
{
	if (op.type == X86_OP_REG && isZmmRegister(op.reg))
	{
		bits = 512;
		auto* i512 = irb.getIntNTy(512);
		llvm::Value* lo = irb.CreateZExt(loadRegister(vectorLow128(op.reg), irb), i512);
		llvm::Value* mid = irb.CreateZExt(loadRegister(vectorMid128(op.reg), irb), i512);
		llvm::Value* hi = irb.CreateZExt(loadRegister(vectorHigh256(op.reg), irb), i512);
		return irb.CreateOr(
			irb.CreateOr(lo, irb.CreateShl(mid, llvm::ConstantInt::get(i512, 128))),
			irb.CreateShl(hi, llvm::ConstantInt::get(i512, 256)));
	}

	if (op.type == X86_OP_REG && isYmmRegister(op.reg))
	{
		bits = 256;
		auto* i256 = irb.getIntNTy(256);
		llvm::Value* lo = irb.CreateZExt(loadRegister(vectorLow128(op.reg), irb), i256);
		llvm::Value* hi = irb.CreateZExt(loadRegister(vectorMid128(op.reg), irb), i256);
		return irb.CreateOr(lo, irb.CreateShl(hi, llvm::ConstantInt::get(i256, 128)));
	}

	if (op.type == X86_OP_REG && isXmmRegister(op.reg))
	{
		bits = 128;
		return loadRegister(vectorLow128(op.reg), irb);
	}

	if (op.type == X86_OP_MEM)
	{
		// The caller worked `bits` out from the REGISTER operands, and for a
		// plain load the memory operand agrees. For an EVEX broadcast it does
		// not: `vpaddd zmm2, zmm1, dword ptr [rdi]{1to16}` reads 32 bits and
		// repeats them sixteen times, and capstone reports that as `size = 4`
		// while leaving avx_bcast at 0 -- the {1to16} shows up in op_str and
		// nowhere else. Taking the register width on trust would read 512
		// bits of memory that the instruction never touches.
		if (op.size * 8 != bits)
		{
			return nullptr;
		}
		auto* ty = irb.getIntNTy(bits);
		return loadOp(op, irb, ty);
	}

	return nullptr;
}

/**
 * Write a vector result back, applying the rule that makes AVX and SSE coexist.
 *
 * A VEX-encoded 128-bit write ZEROES the upper half of the destination; a
 * legacy SSE write leaves it alone. That is the entire reason vzeroupper
 * exists, and it is the one thing a translator can get wrong here without
 * producing a visibly odd value: the low half is right either way, and the
 * upper half only matters to the next 256-bit read.
 */
void Capstone2LlvmIrTranslatorX86_impl::storeVectorOp(
	cs_x86_op& op, llvm::Value* val, unsigned bits, llvm::IRBuilder<>& irb)
{
	if (op.type == X86_OP_REG && isZmmRegister(op.reg))
	{
		auto* i128 = irb.getInt128Ty();
		auto* i256 = irb.getIntNTy(256);
		auto* wide = irb.getIntNTy(512);
		val = irb.CreateZExtOrTrunc(val, wide);
		storeRegister(vectorLow128(op.reg), irb.CreateTrunc(val, i128), irb);
		storeRegister(
			vectorMid128(op.reg), irb.CreateTrunc(irb.CreateLShr(val, llvm::ConstantInt::get(wide, 128)), i128), irb);
		storeRegister(
			vectorHigh256(op.reg), irb.CreateTrunc(irb.CreateLShr(val, llvm::ConstantInt::get(wide, 256)), i256), irb);
		return;
	}

	if (op.type == X86_OP_REG && isYmmRegister(op.reg))
	{
		auto* i128 = irb.getInt128Ty();
		auto* i256 = irb.getIntNTy(256);
		auto* wide = irb.getIntNTy(256);
		val = irb.CreateZExtOrTrunc(val, wide);
		storeRegister(vectorLow128(op.reg), irb.CreateTrunc(val, i128), irb);
		storeRegister(
			vectorMid128(op.reg), irb.CreateTrunc(irb.CreateLShr(val, llvm::ConstantInt::get(wide, 128)), i128), irb);
		// A 256-bit write zeroes bits 511:256, the same rule one level up.
		storeRegister(vectorHigh256(op.reg), llvm::ConstantInt::get(i256, 0), irb);
		return;
	}

	if (op.type == X86_OP_REG && isXmmRegister(op.reg))
	{
		auto* i128 = irb.getInt128Ty();
		auto* i256 = irb.getIntNTy(256);
		storeRegister(vectorLow128(op.reg), irb.CreateZExtOrTrunc(val, i128), irb);
		// The VEX or EVEX 128-bit write zeroes everything above it.
		storeRegister(vectorMid128(op.reg), llvm::ConstantInt::get(i128, 0), irb);
		storeRegister(vectorHigh256(op.reg), llvm::ConstantInt::get(i256, 0), irb);
		return;
	}

	storeOp(op, irb.CreateZExtOrTrunc(val, irb.getIntNTy(bits)), irb, eOpConv::NOTHING);
}

/**
 * True when an instruction carries an EVEX modifier this translator does not
 * model, and must therefore fall back rather than answer the unmodified form.
 *
 * Capstone surfaces a write mask as an EXTRA OPERAND rather than a flag:
 * `vmovdqu8 zmm1{k2}, [rdi]` comes back with three operands, the second being
 * k2, and `{z}` sets avx_zero_opmask on it. That is what makes the check
 * cheap -- an opmask register anywhere in an instruction that is not itself
 * an opmask instruction means the write is predicated.
 *
 * Suppression (`{sae}`) and an embedded rounding mode change what a
 * floating-point result is, so they disqualify an instruction too.
 *
 * @param from First operand to consider. A comparison whose DESTINATION is
 *             an opmask register is not thereby predicated, so those pass 1.
 */
/**
 * 512, 256 or 128 for a vector register name, 0 for anything else.
 */
unsigned Capstone2LlvmIrTranslatorX86_impl::vectorRegisterWidth(uint32_t r) const
{
	return isZmmRegister(r) ? 512 : (isYmmRegister(r) ? 256 : (isXmmRegister(r) ? 128 : 0));
}

bool Capstone2LlvmIrTranslatorX86_impl::hasEvexModifier(cs_x86* xi, unsigned from) const
{
	if (xi->avx_sae || xi->avx_rm != X86_AVX_RM_INVALID)
	{
		return true;
	}

	// `from` skips the destination for an instruction whose destination IS an
	// opmask register -- a compare into k1 is not a predicated instruction.
	for (unsigned k = from; k < xi->op_count; ++k)
	{
		auto& op = xi->operands[k];
		if (op.avx_zero_opmask)
		{
			return true;
		}
		if (op.type == X86_OP_REG && X86_REG_K0 <= op.reg && op.reg <= X86_REG_K7)
		{
			return true;
		}
	}

	return false;
}

/**
 * The width an AVX instruction operates at, taken from its register operands.
 * Zero when they do not agree or are not vector registers at all.
 */
unsigned Capstone2LlvmIrTranslatorX86_impl::avxWidth(cs_x86* xi) const
{
	if (hasEvexModifier(xi))
	{
		return 0;
	}

	unsigned bits = 0;
	for (unsigned k = 0; k < xi->op_count; ++k)
	{
		auto& op = xi->operands[k];
		if (op.type != X86_OP_REG)
		{
			continue;
		}
		unsigned b = isZmmRegister(op.reg) ? 512 : isYmmRegister(op.reg) ? 256 : isXmmRegister(op.reg) ? 128 : 0;
		if (b == 0 || (bits != 0 && b != bits))
		{
			return 0;
		}
		bits = b;
	}

	// Every memory operand has to be the same width the registers agreed on.
	// An EVEX broadcast is the case where it is not, and capstone reports it
	// only in the operand's size -- avx_bcast stays 0 and the {1to16} lives
	// in op_str. Answering the non-broadcast form would read the wrong bytes
	// and repeat none of them.
	for (unsigned k = 0; k < xi->op_count; ++k)
	{
		auto& op = xi->operands[k];
		if (op.type == X86_OP_MEM && op.size * 8 != bits)
		{
			return 0;
		}
	}

	return bits;
}

/**
 * X86_INS_VZEROUPPER, X86_INS_VZEROALL
 *
 * 9,336 occurrences in the static parity corpus, and until the upper halves
 * were registers there was nothing for it to do. It is the instruction that
 * exists because legacy SSE writes leave YMM's upper half alone: a compiler
 * emits it before calling into code that might be SSE, so that the stale
 * uppers do not cost a pipeline stall. Modelling it as a call to an undefined
 * function left every subsequent 256-bit read of those registers wrong.
 */
void Capstone2LlvmIrTranslatorX86_impl::translateVzeroupper(cs_insn* i, cs_x86* xi, llvm::IRBuilder<>& irb)
{
	auto* i128 = irb.getInt128Ty();
	auto* zero = llvm::ConstantInt::get(i128, 0);
	auto* zero256 = llvm::ConstantInt::get(irb.getIntNTy(256), 0);

	// In 64-bit mode both instructions touch registers 0..15 only; EVEX's
	// upper sixteen are explicitly left alone by the manual.
	for (unsigned n = 0; n < 16; ++n)
	{
		storeRegister(X86_REG_YMM0_HI + n, zero, irb);
		storeRegister(X86_REG_ZMM0_HI + n, zero256, irb);
		if (i->id == X86_INS_VZEROALL)
		{
			// VZEROALL zeroes the whole register, not just the upper part.
			storeRegister(X86_REG_XMM0 + n, zero, irb);
		}
	}
}

/**
 * X86_INS_VMOVDQU, X86_INS_VMOVDQA, X86_INS_VMOVUPS, X86_INS_VMOVAPS,
 * X86_INS_VMOVUPD, X86_INS_VMOVAPD, X86_INS_VMOVNTDQ
 *
 * The AVX moves, and between them the largest single group in what is left of
 * x86-64: vmovdqu alone is 21,306 occurrences across its two signatures.
 * Aligned and unaligned differ only in whether a misaligned address faults,
 * which this model does not represent either way; integer and floating-point
 * differ only in which execution unit the hardware uses.
 */
void Capstone2LlvmIrTranslatorX86_impl::translateAvxMov(cs_insn* i, cs_x86* xi, llvm::IRBuilder<>& irb)
{
	if (xi->op_count != 2)
	{
		translatePseudoAsmGeneric(i, xi, irb);
		return;
	}

	unsigned bits = avxWidth(xi);
	if (bits == 0)
	{
		translatePseudoAsmGeneric(i, xi, irb);
		return;
	}

	llvm::Value* src = loadVectorOp(xi->operands[1], irb, bits);
	if (src == nullptr)
	{
		translatePseudoAsmGeneric(i, xi, irb);
		return;
	}

	storeVectorOp(xi->operands[0], src, bits, irb);
}

/**
 * X86_INS_VPAND, X86_INS_VPOR, X86_INS_VPXOR, X86_INS_VPANDN,
 * X86_INS_VPADDB, X86_INS_VPADDW, X86_INS_VPADDD, X86_INS_VPADDQ,
 * X86_INS_VPSUBB, X86_INS_VPSUBW, X86_INS_VPSUBD, X86_INS_VPSUBQ,
 * X86_INS_VPMINUB, X86_INS_VPMAXUB, X86_INS_VPMINSB, X86_INS_VPMAXSB,
 * X86_INS_VPCMPEQB, X86_INS_VPCMPEQW, X86_INS_VPCMPEQD, X86_INS_VPCMPEQQ,
 * X86_INS_VPCMPGTB, X86_INS_VPCMPGTW, X86_INS_VPCMPGTD, X86_INS_VPCMPGTQ
 *
 * The three-operand VEX forms of the packed integer operations. The
 * non-destructive third operand is the whole reason VEX exists, and it is the
 * only structural difference from the SSE versions: `vpxor ymm0, ymm1, ymm2`
 * leaves ymm1 alone where `pxor xmm0, xmm1` does not.
 *
 * The bitwise operations do not care about the lane width; the arithmetic and
 * the compares do, and they are the ones where getting it wrong is quiet --
 * `vpaddb` and `vpaddd` differ only in whether a carry crosses a byte
 * boundary, so any operand whose lanes do not carry gives the same answer for
 * both.
 */
void Capstone2LlvmIrTranslatorX86_impl::translateAvxPackedBinary(cs_insn* i, cs_x86* xi, llvm::IRBuilder<>& irb)
{
	if (xi->op_count != 3)
	{
		translatePseudoAsmGeneric(i, xi, irb);
		return;
	}

	unsigned bits = avxWidth(xi);
	if (bits == 0)
	{
		translatePseudoAsmGeneric(i, xi, irb);
		return;
	}

	unsigned laneBits = 0;
	switch (i->id)
	{
	case X86_INS_VPADDB:
	case X86_INS_VPSUBB:
	case X86_INS_VPMINUB:
	case X86_INS_VPMAXUB:
	case X86_INS_VPMINSB:
	case X86_INS_VPMAXSB:
	case X86_INS_VPCMPEQB:
	case X86_INS_VPCMPGTB: laneBits = 8; break;
	case X86_INS_VPADDW:
	case X86_INS_VPSUBW:
	case X86_INS_VPMINSW:
	case X86_INS_VPMAXSW:
	case X86_INS_VPMINUW:
	case X86_INS_VPMAXUW:
	case X86_INS_VPCMPEQW:
	case X86_INS_VPCMPGTW: laneBits = 16; break;
	case X86_INS_VPADDD:
	case X86_INS_VPSUBD:
	case X86_INS_VPMINSD:
	case X86_INS_VPMAXSD:
	case X86_INS_VPMINUD:
	case X86_INS_VPMAXUD:
	case X86_INS_VPCMPEQD:
	case X86_INS_VPCMPGTD: laneBits = 32; break;
	case X86_INS_VPADDQ:
	case X86_INS_VPSUBQ:
	case X86_INS_VPCMPEQQ:
	case X86_INS_VPCMPGTQ: laneBits = 64; break;
	// The bitwise operations are lane-independent.
	case X86_INS_VPMINSQ:
	case X86_INS_VPMAXSQ:
	case X86_INS_VPMINUQ:
	case X86_INS_VPMAXUQ: laneBits = 64; break;
	case X86_INS_VPAND:
	case X86_INS_VPANDD:
	case X86_INS_VPANDQ:
	case X86_INS_VPOR:
	case X86_INS_VPORD:
	case X86_INS_VPORQ:
	case X86_INS_VPXOR:
	case X86_INS_VPXORD:
	case X86_INS_VPXORQ:
	case X86_INS_VPANDN:
	case X86_INS_VPANDND:
	case X86_INS_VPANDNQ: laneBits = bits; break;
	default: translatePseudoAsmGeneric(i, xi, irb); return;
	}

	unsigned srcBits = bits;
	llvm::Value* a = loadVectorOp(xi->operands[1], irb, srcBits);
	srcBits = bits;
	llvm::Value* b = loadVectorOp(xi->operands[2], irb, srcBits);
	if (a == nullptr || b == nullptr)
	{
		translatePseudoAsmGeneric(i, xi, irb);
		return;
	}

	auto* wide = irb.getIntNTy(bits);
	a = irb.CreateZExtOrTrunc(a, wide);
	b = irb.CreateZExtOrTrunc(b, wide);

	llvm::Value* res = nullptr;
	if (laneBits == bits)
	{
		switch (i->id)
		{
		case X86_INS_VPAND:
		case X86_INS_VPANDD:
		case X86_INS_VPANDQ: res = irb.CreateAnd(a, b); break;
		case X86_INS_VPOR:
		case X86_INS_VPORD:
		case X86_INS_VPORQ: res = irb.CreateOr(a, b); break;
		case X86_INS_VPXOR:
		case X86_INS_VPXORD:
		case X86_INS_VPXORQ: res = irb.CreateXor(a, b); break;
		// ANDN is the FIRST operand inverted, not the second.
		case X86_INS_VPANDN:
		case X86_INS_VPANDND:
		case X86_INS_VPANDNQ: res = irb.CreateAnd(irb.CreateNot(a), b); break;
		default: break;
		}
	}
	else
	{
		unsigned lanes = bits / laneBits;
		auto* vecTy = llvm::FixedVectorType::get(irb.getIntNTy(laneBits), lanes);
		llvm::Value* va = irb.CreateBitCast(a, vecTy);
		llvm::Value* vb = irb.CreateBitCast(b, vecTy);
		llvm::Value* v = nullptr;
		switch (i->id)
		{
		case X86_INS_VPADDB:
		case X86_INS_VPADDW:
		case X86_INS_VPADDD:
		case X86_INS_VPADDQ: v = irb.CreateAdd(va, vb); break;
		case X86_INS_VPSUBB:
		case X86_INS_VPSUBW:
		case X86_INS_VPSUBD:
		case X86_INS_VPSUBQ: v = irb.CreateSub(va, vb); break;
		case X86_INS_VPMINUB: v = irb.CreateSelect(irb.CreateICmpULT(va, vb), va, vb); break;
		case X86_INS_VPMAXUB: v = irb.CreateSelect(irb.CreateICmpUGT(va, vb), va, vb); break;
		case X86_INS_VPMINSB: v = irb.CreateSelect(irb.CreateICmpSLT(va, vb), va, vb); break;
		case X86_INS_VPMAXSB: v = irb.CreateSelect(irb.CreateICmpSGT(va, vb), va, vb); break;
		case X86_INS_VPMINUW: v = irb.CreateSelect(irb.CreateICmpULT(va, vb), va, vb); break;
		case X86_INS_VPMAXUW: v = irb.CreateSelect(irb.CreateICmpUGT(va, vb), va, vb); break;
		case X86_INS_VPMINSW: v = irb.CreateSelect(irb.CreateICmpSLT(va, vb), va, vb); break;
		case X86_INS_VPMAXSW: v = irb.CreateSelect(irb.CreateICmpSGT(va, vb), va, vb); break;
		case X86_INS_VPMINUD: v = irb.CreateSelect(irb.CreateICmpULT(va, vb), va, vb); break;
		case X86_INS_VPMAXUD: v = irb.CreateSelect(irb.CreateICmpUGT(va, vb), va, vb); break;
		case X86_INS_VPMINSD: v = irb.CreateSelect(irb.CreateICmpSLT(va, vb), va, vb); break;
		case X86_INS_VPMAXSD: v = irb.CreateSelect(irb.CreateICmpSGT(va, vb), va, vb); break;
		case X86_INS_VPMINUQ: v = irb.CreateSelect(irb.CreateICmpULT(va, vb), va, vb); break;
		case X86_INS_VPMAXUQ: v = irb.CreateSelect(irb.CreateICmpUGT(va, vb), va, vb); break;
		case X86_INS_VPMINSQ: v = irb.CreateSelect(irb.CreateICmpSLT(va, vb), va, vb); break;
		case X86_INS_VPMAXSQ: v = irb.CreateSelect(irb.CreateICmpSGT(va, vb), va, vb); break;
		case X86_INS_VPCMPEQB:
		case X86_INS_VPCMPEQW:
		case X86_INS_VPCMPEQD:
		case X86_INS_VPCMPEQQ:
			// A lane of the result is all ones or all zeroes.
			v = irb.CreateSExt(irb.CreateICmpEQ(va, vb), vecTy);
			break;
		case X86_INS_VPCMPGTB:
		case X86_INS_VPCMPGTW:
		case X86_INS_VPCMPGTD:
		case X86_INS_VPCMPGTQ:
			// SIGNED greater-than; there is no unsigned form.
			v = irb.CreateSExt(irb.CreateICmpSGT(va, vb), vecTy);
			break;
		default: break;
		}
		if (v != nullptr)
		{
			res = irb.CreateBitCast(v, wide);
		}
	}

	if (res == nullptr)
	{
		translatePseudoAsmGeneric(i, xi, irb);
		return;
	}

	storeVectorOp(xi->operands[0], res, bits, irb);
}

/**
 * X86_INS_VPMOVMSKB
 *
 * 19,778 occurrences, the second-largest single entry left on x86-64, and the
 * instruction every AVX2 string routine ends with: it collects the TOP BIT of
 * each byte lane into a general-purpose register, so that a lane compare
 * becomes a bitmask a scalar `tzcnt` can search.
 *
 * Sixteen bits from an XMM source and thirty-two from a YMM one, and the
 * destination is always a 32-bit register with the rest zeroed. Taking the low
 * bit of each lane instead of the top one is the quiet way to get this wrong:
 * for the all-ones and all-zeroes lanes a compare produces, the two readings
 * agree exactly, which is why the test here uses lanes that are neither.
 */
void Capstone2LlvmIrTranslatorX86_impl::translateAvxPmovmskb(cs_insn* i, cs_x86* xi, llvm::IRBuilder<>& irb)
{
	if (xi->op_count != 2 || xi->operands[1].type != X86_OP_REG)
	{
		translatePseudoAsmGeneric(i, xi, irb);
		return;
	}

	unsigned bits = isYmmRegister(xi->operands[1].reg) ? 256 : (isXmmRegister(xi->operands[1].reg) ? 128 : 0);
	if (bits == 0)
	{
		translatePseudoAsmGeneric(i, xi, irb);
		return;
	}

	llvm::Value* src = loadVectorOp(xi->operands[1], irb, bits);
	if (src == nullptr)
	{
		translatePseudoAsmGeneric(i, xi, irb);
		return;
	}

	unsigned lanes = bits / 8;
	auto* vecTy = llvm::FixedVectorType::get(irb.getInt8Ty(), lanes);
	llvm::Value* v = irb.CreateBitCast(src, vecTy);

	// The sign bit of each lane, gathered into the bottom `lanes` bits.
	llvm::Value* signs = irb.CreateICmpSLT(v, llvm::Constant::getNullValue(vecTy));
	llvm::Value* mask = irb.CreateBitCast(signs, irb.getIntNTy(lanes));

	storeOp(xi->operands[0], irb.CreateZExt(mask, irb.getInt32Ty()), irb, eOpConv::ZEXT_TRUNC_OR_BITCAST);
}

/**
 * X86_INS_VMOVD, X86_INS_VMOVQ
 *
 * The scalar moves between a general-purpose register, memory and the bottom
 * of a vector register. They are not narrow versions of VMOVDQU: every form
 * that writes a vector register ZEROES everything above the element it
 * writes, including the `vmovq xmm2, xmm1` form, whose whole purpose is to
 * take the low 64 bits of one register and clear the rest.
 *
 * Routing these through translateAvxMov would copy the full 128 bits for the
 * register-to-register form and leave the upper half of the destination
 * carrying whatever it had for the others.
 */
void Capstone2LlvmIrTranslatorX86_impl::translateAvxMovScalar(cs_insn* i, cs_x86* xi, llvm::IRBuilder<>& irb)
{
	if (xi->op_count != 2 || hasEvexModifier(xi))
	{
		translatePseudoAsmGeneric(i, xi, irb);
		return;
	}

	unsigned elemBits = i->id == X86_INS_VMOVQ ? 64 : 32;
	auto* elemTy = irb.getIntNTy(elemBits);
	auto& dst = xi->operands[0];
	auto& src = xi->operands[1];

	// Reading the element: from the bottom of a vector register, or from a
	// general-purpose register or memory operand at its own width.
	llvm::Value* val = nullptr;
	if (src.type == X86_OP_REG && isXmmRegister(src.reg))
	{
		val = irb.CreateTrunc(loadRegister(vectorLow128(src.reg), irb), elemTy);
	}
	else if (src.type == X86_OP_MEM && src.size * 8 != elemBits)
	{
		translatePseudoAsmGeneric(i, xi, irb);
		return;
	}
	else
	{
		val = loadOp(src, irb, elemTy, false);
	}

	if (val == nullptr)
	{
		translatePseudoAsmGeneric(i, xi, irb);
		return;
	}

	if (dst.type == X86_OP_REG && isXmmRegister(dst.reg))
	{
		// Zero-extend into the low 128 and clear everything above it. This is
		// the one rule all of these share and the one a whole-register move
		// gets wrong.
		auto* i128 = irb.getInt128Ty();
		auto* i256 = irb.getIntNTy(256);
		storeRegister(vectorLow128(dst.reg), irb.CreateZExt(val, i128), irb);
		storeRegister(vectorMid128(dst.reg), llvm::ConstantInt::get(i128, 0), irb);
		storeRegister(vectorHigh256(dst.reg), llvm::ConstantInt::get(i256, 0), irb);
		return;
	}

	if (dst.type == X86_OP_MEM && dst.size * 8 != elemBits)
	{
		translatePseudoAsmGeneric(i, xi, irb);
		return;
	}

	storeOp(dst, val, irb, eOpConv::ZEXT_TRUNC_OR_BITCAST);
}

/**
 * X86_INS_VADDPS and the rest of the VEX/EVEX packed floating-point
 * arithmetic, at 128, 256 or 512 bits.
 *
 * These are the instructions that make hasEvexModifier()'s {sae} and
 * embedded-rounding-mode branch load-bearing rather than defensive: an EVEX
 * `vaddps {rn-sae}, %zmm1, %zmm2, %zmm3` has three plain register operands
 * and no write mask, so nothing about its shape distinguishes it from the
 * ordinary form -- only avx_sae and avx_rm do. Suppressing exceptions and
 * pinning the rounding mode change what the result is, and an IEEE `fadd`
 * expresses neither, so the correct answer is to decline.
 */
void Capstone2LlvmIrTranslatorX86_impl::translateAvxPackedFloat(cs_insn* i, cs_x86* xi, llvm::IRBuilder<>& irb)
{
	bool isDouble = false;
	unsigned op = 0;
	bool isSqrt = false;
	switch (i->id)
	{
	case X86_INS_VADDPS: op = 0; break;
	case X86_INS_VSUBPS: op = 1; break;
	case X86_INS_VMULPS: op = 2; break;
	case X86_INS_VDIVPS: op = 3; break;
	case X86_INS_VSQRTPS: isSqrt = true; break;
	case X86_INS_VADDPD:
		op = 0;
		isDouble = true;
		break;
	case X86_INS_VSUBPD:
		op = 1;
		isDouble = true;
		break;
	case X86_INS_VMULPD:
		op = 2;
		isDouble = true;
		break;
	case X86_INS_VDIVPD:
		op = 3;
		isDouble = true;
		break;
	case X86_INS_VSQRTPD:
		isSqrt = true;
		isDouble = true;
		break;
	default: translatePseudoAsmGeneric(i, xi, irb); return;
	}

	if (isSqrt)
	{
		if (xi->op_count != 2)
		{
			translatePseudoAsmGeneric(i, xi, irb);
			return;
		}
		unsigned bits = avxWidth(xi);
		if (bits == 0)
		{
			translatePseudoAsmGeneric(i, xi, irb);
			return;
		}
		llvm::Value* a = loadVectorOp(xi->operands[1], irb, bits);
		if (a == nullptr)
		{
			translatePseudoAsmGeneric(i, xi, irb);
			return;
		}
		unsigned laneBits = isDouble ? 64 : 32;
		auto* laneTy = isDouble ? irb.getDoubleTy() : irb.getFloatTy();
		auto* vecTy = llvm::FixedVectorType::get(laneTy, bits / laneBits);
		llvm::Value* va = irb.CreateBitCast(a, vecTy);
		storeVectorOp(
			xi->operands[0],
			irb.CreateBitCast(irb.CreateUnaryIntrinsic(llvm::Intrinsic::sqrt, va), irb.getIntNTy(bits)),
			bits,
			irb);
		return;
	}

	if (xi->op_count != 3)
	{
		translatePseudoAsmGeneric(i, xi, irb);
		return;
	}

	unsigned bits = avxWidth(xi);
	if (bits == 0)
	{
		translatePseudoAsmGeneric(i, xi, irb);
		return;
	}

	llvm::Value* a = loadVectorOp(xi->operands[1], irb, bits);
	llvm::Value* b = loadVectorOp(xi->operands[2], irb, bits);
	if (a == nullptr || b == nullptr)
	{
		translatePseudoAsmGeneric(i, xi, irb);
		return;
	}

	unsigned laneBits = isDouble ? 64 : 32;
	auto* laneTy = isDouble ? irb.getDoubleTy() : irb.getFloatTy();
	auto* vecTy = llvm::FixedVectorType::get(laneTy, bits / laneBits);

	llvm::Value* va = irb.CreateBitCast(a, vecTy);
	llvm::Value* vb = irb.CreateBitCast(b, vecTy);
	llvm::Value* res = nullptr;
	switch (op)
	{
	case 0: res = irb.CreateFAdd(va, vb); break;
	case 1: res = irb.CreateFSub(va, vb); break;
	case 2: res = irb.CreateFMul(va, vb); break;
	default: res = irb.CreateFDiv(va, vb); break;
	}

	storeVectorOp(xi->operands[0], irb.CreateBitCast(res, irb.getIntNTy(bits)), bits, irb);
}

/**
 * FMA3 (132/213/231) and FMA4 packed/scalar fused multiply-add.
 *
 * Intel SDM operand order for dest, src1, src2:
 *   132: dest = dest * src2 + src1
 *   213: dest = src1 * dest + src2
 *   231: dest = src1 * src2 + dest
 * FMA4 (no number): dest = src1 * src2 + src3
 *
 * llvm.fma(a, b, c) is a*b+c. VFMSUB negates the addend, VFNMADD negates a
 * factor of the product, VFNMSUB both. VFMADDSUB subtracts even lanes and
 * adds odd ones; VFMSUBADD is the reverse.
 */
void Capstone2LlvmIrTranslatorX86_impl::translateAvxFma(cs_insn* i, cs_x86* xi, llvm::IRBuilder<>& irb)
{
	enum Order
	{
		F132,
		F213,
		F231,
		F4
	};
	enum Acc
	{
		Add,
		Sub,
		NAdd,
		NSub,
		AddSub,
		SubAdd
	};
	Order order = F231;
	Acc acc = Add;
	bool isDouble = false;
	bool isScalar = false;
	switch (i->id)
	{
	case X86_INS_VFMADD132PS: order = F132; break;
	case X86_INS_VFMADD213PS: order = F213; break;
	case X86_INS_VFMADD231PS: order = F231; break;
	case X86_INS_VFMADDPS: order = F4; break;
	case X86_INS_VFMADD132PD: order = F132; isDouble = true; break;
	case X86_INS_VFMADD213PD: order = F213; isDouble = true; break;
	case X86_INS_VFMADD231PD: order = F231; isDouble = true; break;
	case X86_INS_VFMADDPD: order = F4; isDouble = true; break;
	case X86_INS_VFMADD132SS: order = F132; isScalar = true; break;
	case X86_INS_VFMADD213SS: order = F213; isScalar = true; break;
	case X86_INS_VFMADD231SS: order = F231; isScalar = true; break;
	case X86_INS_VFMADDSS: order = F4; isScalar = true; break;
	case X86_INS_VFMADD132SD: order = F132; isDouble = true; isScalar = true; break;
	case X86_INS_VFMADD213SD: order = F213; isDouble = true; isScalar = true; break;
	case X86_INS_VFMADD231SD: order = F231; isDouble = true; isScalar = true; break;
	case X86_INS_VFMADDSD: order = F4; isDouble = true; isScalar = true; break;
	case X86_INS_VFMSUB132PS: order = F132; acc = Sub; break;
	case X86_INS_VFMSUB213PS: order = F213; acc = Sub; break;
	case X86_INS_VFMSUB231PS: order = F231; acc = Sub; break;
	case X86_INS_VFMSUBPS: order = F4; acc = Sub; break;
	case X86_INS_VFMSUB132PD: order = F132; acc = Sub; isDouble = true; break;
	case X86_INS_VFMSUB213PD: order = F213; acc = Sub; isDouble = true; break;
	case X86_INS_VFMSUB231PD: order = F231; acc = Sub; isDouble = true; break;
	case X86_INS_VFMSUBPD: order = F4; acc = Sub; isDouble = true; break;
	case X86_INS_VFMSUB132SS: order = F132; acc = Sub; isScalar = true; break;
	case X86_INS_VFMSUB213SS: order = F213; acc = Sub; isScalar = true; break;
	case X86_INS_VFMSUB231SS: order = F231; acc = Sub; isScalar = true; break;
	case X86_INS_VFMSUBSS: order = F4; acc = Sub; isScalar = true; break;
	case X86_INS_VFMSUB132SD: order = F132; acc = Sub; isDouble = true; isScalar = true; break;
	case X86_INS_VFMSUB213SD: order = F213; acc = Sub; isDouble = true; isScalar = true; break;
	case X86_INS_VFMSUB231SD: order = F231; acc = Sub; isDouble = true; isScalar = true; break;
	case X86_INS_VFMSUBSD: order = F4; acc = Sub; isDouble = true; isScalar = true; break;
	case X86_INS_VFNMADD132PS: order = F132; acc = NAdd; break;
	case X86_INS_VFNMADD213PS: order = F213; acc = NAdd; break;
	case X86_INS_VFNMADD231PS: order = F231; acc = NAdd; break;
	case X86_INS_VFNMADDPS: order = F4; acc = NAdd; break;
	case X86_INS_VFNMADD132PD: order = F132; acc = NAdd; isDouble = true; break;
	case X86_INS_VFNMADD213PD: order = F213; acc = NAdd; isDouble = true; break;
	case X86_INS_VFNMADD231PD: order = F231; acc = NAdd; isDouble = true; break;
	case X86_INS_VFNMADDPD: order = F4; acc = NAdd; isDouble = true; break;
	case X86_INS_VFNMADD132SS: order = F132; acc = NAdd; isScalar = true; break;
	case X86_INS_VFNMADD213SS: order = F213; acc = NAdd; isScalar = true; break;
	case X86_INS_VFNMADD231SS: order = F231; acc = NAdd; isScalar = true; break;
	case X86_INS_VFNMADDSS: order = F4; acc = NAdd; isScalar = true; break;
	case X86_INS_VFNMADD132SD: order = F132; acc = NAdd; isDouble = true; isScalar = true; break;
	case X86_INS_VFNMADD213SD: order = F213; acc = NAdd; isDouble = true; isScalar = true; break;
	case X86_INS_VFNMADD231SD: order = F231; acc = NAdd; isDouble = true; isScalar = true; break;
	case X86_INS_VFNMADDSD: order = F4; acc = NAdd; isDouble = true; isScalar = true; break;
	case X86_INS_VFNMSUB132PS: order = F132; acc = NSub; break;
	case X86_INS_VFNMSUB213PS: order = F213; acc = NSub; break;
	case X86_INS_VFNMSUB231PS: order = F231; acc = NSub; break;
	case X86_INS_VFNMSUBPS: order = F4; acc = NSub; break;
	case X86_INS_VFNMSUB132PD: order = F132; acc = NSub; isDouble = true; break;
	case X86_INS_VFNMSUB213PD: order = F213; acc = NSub; isDouble = true; break;
	case X86_INS_VFNMSUB231PD: order = F231; acc = NSub; isDouble = true; break;
	case X86_INS_VFNMSUBPD: order = F4; acc = NSub; isDouble = true; break;
	case X86_INS_VFNMSUB132SS: order = F132; acc = NSub; isScalar = true; break;
	case X86_INS_VFNMSUB213SS: order = F213; acc = NSub; isScalar = true; break;
	case X86_INS_VFNMSUB231SS: order = F231; acc = NSub; isScalar = true; break;
	case X86_INS_VFNMSUBSS: order = F4; acc = NSub; isScalar = true; break;
	case X86_INS_VFNMSUB132SD: order = F132; acc = NSub; isDouble = true; isScalar = true; break;
	case X86_INS_VFNMSUB213SD: order = F213; acc = NSub; isDouble = true; isScalar = true; break;
	case X86_INS_VFNMSUB231SD: order = F231; acc = NSub; isDouble = true; isScalar = true; break;
	case X86_INS_VFNMSUBSD: order = F4; acc = NSub; isDouble = true; isScalar = true; break;
	case X86_INS_VFMADDSUB132PS: order = F132; acc = AddSub; break;
	case X86_INS_VFMADDSUB213PS: order = F213; acc = AddSub; break;
	case X86_INS_VFMADDSUB231PS: order = F231; acc = AddSub; break;
	case X86_INS_VFMADDSUBPS: order = F4; acc = AddSub; break;
	case X86_INS_VFMADDSUB132PD: order = F132; acc = AddSub; isDouble = true; break;
	case X86_INS_VFMADDSUB213PD: order = F213; acc = AddSub; isDouble = true; break;
	case X86_INS_VFMADDSUB231PD: order = F231; acc = AddSub; isDouble = true; break;
	case X86_INS_VFMADDSUBPD: order = F4; acc = AddSub; isDouble = true; break;
	case X86_INS_VFMSUBADD132PS: order = F132; acc = SubAdd; break;
	case X86_INS_VFMSUBADD213PS: order = F213; acc = SubAdd; break;
	case X86_INS_VFMSUBADD231PS: order = F231; acc = SubAdd; break;
	case X86_INS_VFMSUBADDPS: order = F4; acc = SubAdd; break;
	case X86_INS_VFMSUBADD132PD: order = F132; acc = SubAdd; isDouble = true; break;
	case X86_INS_VFMSUBADD213PD: order = F213; acc = SubAdd; isDouble = true; break;
	case X86_INS_VFMSUBADD231PD: order = F231; acc = SubAdd; isDouble = true; break;
	case X86_INS_VFMSUBADDPD: order = F4; acc = SubAdd; isDouble = true; break;
	default: translatePseudoAsmGeneric(i, xi, irb); return;
	}

	if (hasEvexModifier(xi))
	{
		translatePseudoAsmGeneric(i, xi, irb);
		return;
	}

	auto fmaCall = [&](Value* a, Value* b, Value* c) {
		if (acc == NAdd || acc == NSub)
		{
			a = irb.CreateFNeg(a);
		}
		if (acc == Sub || acc == NSub)
		{
			c = irb.CreateFNeg(c);
		}
		auto* fn = Intrinsic::getOrInsertDeclaration(_module, Intrinsic::fma, a->getType());
		return irb.CreateCall(fn, {a, b, c});
	};

	auto fmaBlend = [&](Value* a, Value* b, Value* c, unsigned n) {
		Value* added = fmaCall(a, b, c);
		Value* subbed = fmaCall(a, b, irb.CreateFNeg(c));
		// AddSub: even subtract, odd add. SubAdd: even add, odd subtract.
		bool evenSub = (acc == AddSub);
		std::vector<int> mask;
		mask.reserve(n);
		for (unsigned lane = 0; lane < n; ++lane)
		{
			bool subLane = ((lane & 1u) == 0) ? evenSub : !evenSub;
			mask.push_back(static_cast<int>(subLane ? lane : (n + lane)));
		}
		return irb.CreateShuffleVector(subbed, added, mask);
	};

	if (isScalar)
	{
		unsigned need = (order == F4) ? 4 : 3;
		if (xi->op_count != need)
		{
			translatePseudoAsmGeneric(i, xi, irb);
			return;
		}
		Type* elemTy = isDouble ? irb.getDoubleTy() : irb.getFloatTy();
		unsigned n = isDouble ? 2 : 4;
		auto* vecTy = FixedVectorType::get(elemTy, n);
		Value* destV = irb.CreateBitCast(toI128(loadOp(xi->operands[0], irb), irb), vecTy);
		Value* s1 = irb.CreateBitCast(toI128(loadOp(xi->operands[1], irb), irb), vecTy);
		Value* s2 = irb.CreateBitCast(toI128(loadOp(xi->operands[2], irb), irb), vecTy);
		Value* d0 = irb.CreateExtractElement(destV, (uint64_t)0);
		Value* a1 = irb.CreateExtractElement(s1, (uint64_t)0);
		Value* a2 = irb.CreateExtractElement(s2, (uint64_t)0);
		Value* mul1 = nullptr;
		Value* mul2 = nullptr;
		Value* add = nullptr;
		if (order == F4)
		{
			Value* s3 = irb.CreateBitCast(toI128(loadOp(xi->operands[3], irb), irb), vecTy);
			mul1 = a1;
			mul2 = a2;
			add = irb.CreateExtractElement(s3, (uint64_t)0);
		}
		else if (order == F132)
		{
			mul1 = d0;
			mul2 = a2;
			add = a1;
		}
		else if (order == F213)
		{
			mul1 = a1;
			mul2 = d0;
			add = a2;
		}
		else
		{
			mul1 = a1;
			mul2 = a2;
			add = d0;
		}
		Value* r = fmaCall(mul1, mul2, add);
		storeVectorOp(
			xi->operands[0],
			irb.CreateBitCast(irb.CreateInsertElement(destV, r, (uint64_t)0), irb.getInt128Ty()),
			128,
			irb);
		return;
	}

	unsigned need = (order == F4) ? 4 : 3;
	if (xi->op_count != need)
	{
		translatePseudoAsmGeneric(i, xi, irb);
		return;
	}
	unsigned destBits = xi->operands[0].size * 8;
	if (destBits != 128 && destBits != 256)
	{
		translatePseudoAsmGeneric(i, xi, irb);
		return;
	}
	unsigned bits = destBits;
	Value* dest = loadVectorOp(xi->operands[0], irb, bits);
	bits = destBits;
	Value* s1 = loadVectorOp(xi->operands[1], irb, bits);
	bits = destBits;
	Value* s2 = loadVectorOp(xi->operands[2], irb, bits);
	Value* s3 = nullptr;
	if (order == F4)
	{
		bits = destBits;
		s3 = loadVectorOp(xi->operands[3], irb, bits);
	}
	if (dest == nullptr || s1 == nullptr || s2 == nullptr || (order == F4 && s3 == nullptr))
	{
		translatePseudoAsmGeneric(i, xi, irb);
		return;
	}

	unsigned laneBits = isDouble ? 64 : 32;
	unsigned n = destBits / laneBits;
	auto* laneTy = isDouble ? irb.getDoubleTy() : irb.getFloatTy();
	auto* vecTy = llvm::FixedVectorType::get(laneTy, n);
	auto* intTy = irb.getIntNTy(destBits);
	Value* vd = irb.CreateBitCast(irb.CreateZExtOrTrunc(dest, intTy), vecTy);
	Value* v1 = irb.CreateBitCast(irb.CreateZExtOrTrunc(s1, intTy), vecTy);
	Value* v2 = irb.CreateBitCast(irb.CreateZExtOrTrunc(s2, intTy), vecTy);
	Value* mul1 = nullptr;
	Value* mul2 = nullptr;
	Value* add = nullptr;
	if (order == F4)
	{
		mul1 = v1;
		mul2 = v2;
		add = irb.CreateBitCast(irb.CreateZExtOrTrunc(s3, intTy), vecTy);
	}
	else if (order == F132)
	{
		mul1 = vd;
		mul2 = v2;
		add = v1;
	}
	else if (order == F213)
	{
		mul1 = v1;
		mul2 = vd;
		add = v2;
	}
	else
	{
		mul1 = v1;
		mul2 = v2;
		add = vd;
	}
	Value* res = (acc == AddSub || acc == SubAdd) ? fmaBlend(mul1, mul2, add, n) : fmaCall(mul1, mul2, add);
	storeVectorOp(xi->operands[0], irb.CreateBitCast(res, intTy), destBits, irb);
}

//
//==============================================================================
// SSE4.2 string comparison: PCMPISTRI / PCMPISTRM
//==============================================================================
//
// The largest single unmodelled instruction left on x86-64 -- 6,393 sites in
// the static corpus, because glibc's strlen, strcmp, strchr and strstr are all
// built on it. It has a small language in its imm8:
//
//   imm8[1:0]  format      00 unsigned bytes  01 unsigned words
//                          10 signed bytes    11 signed words
//   imm8[3:2]  aggregation 00 EqualAny  01 Ranges  10 EqualEach  11 EqualOrdered
//   imm8[5:4]  polarity    00 positive  01 negative
//                          10 masked positive  11 masked negative
//   imm8[6]    output      0 least significant index  1 most significant
//
// Every one of those is a literal, so all of it is decided here and only the
// data is left to run time.
//
// The semantics below are not from the manual alone. A C model of this
// function was checked against the hardware over 1,760,000 (operand, operand,
// imm8) triples spanning 40 control bytes, every aggregation, both formats,
// both polarities that do anything, and both output selections. It matched on
// every one, including each flag. What is implemented here is that model.
//
// "Implicit length" means each operand ends at its first zero element. The
// validity mask is therefore the bits BELOW the lowest set bit of the
// element-is-zero mask -- `(Z & -Z) - 1`, which is all ones when Z is zero,
// so the no-null case needs no special path.
//

/**
 * The bits strictly below the lowest set bit of @p zeroMask: one bit per
 * element, set while the element is still part of the string.
 *
 * When no element is zero this is all ones, because `0 & -0` is 0 and `0 - 1`
 * is all ones -- the same expression, no branch.
 */
llvm::Value* Capstone2LlvmIrTranslatorX86_impl::generateValidMask(llvm::Value* zeroMask, llvm::IRBuilder<>& irb)
{
	llvm::Value* lowest = irb.CreateAnd(zeroMask, irb.CreateNeg(zeroMask));
	return irb.CreateSub(lowest, llvm::ConstantInt::get(zeroMask->getType(), 1));
}

/// Turn a one-bit-per-element mask into an all-ones-per-element vector mask.
llvm::Value* Capstone2LlvmIrTranslatorX86_impl::spreadMaskToLanes(
	llvm::Value* mask, llvm::FixedVectorType* vecTy, llvm::IRBuilder<>& irb)
{
	unsigned n = vecTy->getNumElements();
	auto* boolVec = llvm::FixedVectorType::get(irb.getInt1Ty(), n);
	return irb.CreateSExt(irb.CreateBitCast(mask, boolVec), vecTy);
}

/// Bit @p bit of @p mask, broadcast to every element of @p vecTy.
llvm::Value* Capstone2LlvmIrTranslatorX86_impl::broadcastMaskBit(
	llvm::Value* mask, unsigned bit, llvm::Type* ty, llvm::IRBuilder<>& irb)
{
	auto* maskTy = llvm::cast<llvm::IntegerType>(mask->getType());
	llvm::Value* b = irb.CreateTrunc(irb.CreateLShr(mask, llvm::ConstantInt::get(maskTy, bit)), irb.getInt1Ty());
	return irb.CreateSExt(b, ty);
}

void Capstone2LlvmIrTranslatorX86_impl::translateStringCompare(cs_insn* i, cs_x86* xi, llvm::IRBuilder<>& irb)
{
	if (xi->op_count != 3 || xi->operands[2].type != X86_OP_IMM)
	{
		translatePseudoAsmGeneric(i, xi, irb);
		return;
	}

	unsigned imm = static_cast<unsigned>(xi->operands[2].imm) & 0xff;
	unsigned fmt = imm & 3;
	unsigned agg = (imm >> 2) & 3;
	unsigned pol = (imm >> 4) & 3;
	bool mostSignificant = ((imm >> 6) & 1) != 0;
	bool wide = (fmt & 1) != 0;
	bool isSigned = ((fmt >> 1) & 1) != 0;
	unsigned n = wide ? 8 : 16;
	unsigned laneBits = wide ? 16 : 8;

	unsigned bits = 128;
	llvm::Value* rawA = loadVectorOp(xi->operands[0], irb, bits);
	bits = 128;
	llvm::Value* rawB = loadVectorOp(xi->operands[1], irb, bits);
	if (rawA == nullptr || rawB == nullptr)
	{
		translatePseudoAsmGeneric(i, xi, irb);
		return;
	}

	auto* vecTy = llvm::FixedVectorType::get(irb.getIntNTy(laneBits), n);
	auto* maskTy = irb.getIntNTy(n);
	auto* zeroVec = llvm::Constant::getNullValue(vecTy);

	llvm::Value* va = irb.CreateBitCast(rawA, vecTy);
	llvm::Value* vb = irb.CreateBitCast(rawB, vecTy);

	// Element-is-zero masks, and from them the validity masks.
	// PCMPESTRI/PCMPESTRM (and the VEX twins) take explicit lengths in
	// EAX/EDX. This translator keeps the implicit-terminator PCMPISTR*
	// approximation and ignores those GPRs.
	llvm::Value* z1 = irb.CreateBitCast(irb.CreateICmpEQ(va, zeroVec), maskTy);
	llvm::Value* z2 = irb.CreateBitCast(irb.CreateICmpEQ(vb, zeroVec), maskTy);
	llvm::Value* v1 = generateValidMask(z1, irb);
	llvm::Value* v2 = generateValidMask(z2, irb);

	llvm::Value* res1 = nullptr;

	if (agg == 0) // EqualAny: does this element of the second operand appear
				  // anywhere in the first?
	{
		// Zeroing the invalid elements of the first operand is enough to
		// exclude them: every VALID element of the second operand is
		// non-zero by construction, so a zero can never match one, and the
		// invalid elements of the second operand are masked off at the end.
		llvm::Value* maskedA = irb.CreateAnd(va, spreadMaskToLanes(v1, vecTy, irb));
		llvm::Value* acc = llvm::ConstantInt::get(maskTy, 0);
		for (unsigned j = 0; j < n; ++j)
		{
			llvm::Value* elem = irb.CreateExtractElement(maskedA, irb.getInt32(j));
			llvm::Value* eq = irb.CreateICmpEQ(vb, irb.CreateVectorSplat(n, elem));
			acc = irb.CreateOr(acc, irb.CreateBitCast(eq, maskTy));
		}
		res1 = irb.CreateAnd(acc, v2);
	}
	else if (agg == 1) // Ranges: the first operand is a list of (low, high)
					   // pairs.
	{
		// The zeroing trick is NOT available here. A signed pair whose high
		// element was zeroed becomes the range [low, 0], which still matches
		// every negative value -- so each pair carries its own validity.
		llvm::Value* acc = llvm::ConstantInt::get(maskTy, 0);
		for (unsigned j = 0; j + 1 < n; j += 2)
		{
			llvm::Value* lo = irb.CreateVectorSplat(n, irb.CreateExtractElement(va, irb.getInt32(j)));
			llvm::Value* hi = irb.CreateVectorSplat(n, irb.CreateExtractElement(va, irb.getInt32(j + 1)));
			llvm::Value* geLo = isSigned ? irb.CreateICmpSGE(vb, lo) : irb.CreateICmpUGE(vb, lo);
			llvm::Value* leHi = isSigned ? irb.CreateICmpSLE(vb, hi) : irb.CreateICmpULE(vb, hi);
			llvm::Value* in = irb.CreateBitCast(irb.CreateAnd(geLo, leHi), maskTy);
			llvm::Value* pairValid =
				irb.CreateAnd(broadcastMaskBit(v1, j, maskTy, irb), broadcastMaskBit(v1, j + 1, maskTy, irb));
			acc = irb.CreateOr(acc, irb.CreateAnd(in, pairValid));
		}
		res1 = irb.CreateAnd(acc, v2);
	}
	else if (agg == 2) // EqualEach: element-wise equality.
	{
		// Two elements that are both PAST the end of their string count as
		// equal -- that forced true is what makes a negative-polarity
		// EqualEach answer "the strings match" with all bits clear.
		llvm::Value* eq = irb.CreateBitCast(irb.CreateICmpEQ(va, vb), maskTy);
		llvm::Value* bothValid = irb.CreateAnd(v1, v2);
		llvm::Value* bothInvalid = irb.CreateAnd(irb.CreateNot(v1), irb.CreateNot(v2));
		res1 = irb.CreateOr(irb.CreateAnd(eq, bothValid), bothInvalid);
	}
	else // EqualOrdered: does the first operand occur at this offset in the
		 // second? -- the substring search.
	{
		llvm::Value* acc = llvm::Constant::getAllOnesValue(maskTy);
		for (unsigned j = 0; j < n; ++j)
		{
			// Lane i of `shifted` is element i+j of the second operand. The
			// lanes where i+j runs off the end are forced to 1 below, so
			// what the shuffle puts there does not matter.
			llvm::SmallVector<int, 16> idx;
			for (unsigned k = 0; k < n; ++k)
			{
				idx.push_back(static_cast<int>(k + j < n ? k + j : n - 1));
			}
			llvm::Value* shifted = irb.CreateShuffleVector(vb, vb, idx);
			llvm::Value* elem = irb.CreateVectorSplat(n, irb.CreateExtractElement(va, irb.getInt32(j)));
			llvm::Value* eq = irb.CreateBitCast(irb.CreateICmpEQ(shifted, elem), maskTy);

			// Element i+j of the second operand must be valid too.
			llvm::Value* v2Shifted = irb.CreateLShr(v2, llvm::ConstantInt::get(maskTy, j));
			// Lanes that ran off the end contribute nothing.
			uint64_t past = (j == 0) ? 0 : (~((1ULL << (n - j)) - 1) & ((1ULL << n) - 1));
			llvm::Value* term = irb.CreateOr(irb.CreateAnd(eq, v2Shifted), llvm::ConstantInt::get(maskTy, past));

			// An invalid element of the first operand ends the comparison:
			// everything from there on counts as matching.
			llvm::Value* aValid = broadcastMaskBit(v1, j, maskTy, irb);
			acc = irb.CreateAnd(acc, irb.CreateOr(irb.CreateNot(aValid), term));
		}
		res1 = acc;
	}

	// Polarity. Only 01 and 11 do anything; 00 and 10 leave the result alone.
	llvm::Value* res2 = res1;
	if (pol == 1)
	{
		res2 = irb.CreateNot(res1);
	}
	else if (pol == 3)
	{
		// Masked negative inverts only where the SECOND operand is still
		// inside its string.
		res2 = irb.CreateXor(res1, v2);
	}

	auto* zeroMask = llvm::ConstantInt::get(maskTy, 0);
	llvm::Value* any = irb.CreateICmpNE(res2, zeroMask);

	if (i->id == X86_INS_PCMPISTRM || i->id == X86_INS_PCMPESTRM
		|| i->id == X86_INS_VPCMPISTRM || i->id == X86_INS_VPCMPESTRM)
	{
		// The mask form writes XMM0: bit 6 chooses between the raw bit mask
		// zero-extended and one all-ones element per set bit.
		auto* i128 = irb.getInt128Ty();
		llvm::Value* out =
			mostSignificant ? irb.CreateBitCast(spreadMaskToLanes(res2, vecTy, irb), i128) : irb.CreateZExt(res2, i128);
		storeRegister(X86_REG_XMM0, out, irb);
		storeRegister(X86_REG_YMM0_HI, llvm::ConstantInt::get(i128, 0), irb);
		storeRegister(X86_REG_ZMM0_HI, llvm::ConstantInt::get(irb.getIntNTy(256), 0), irb);
	}
	else
	{
		// The index form writes ECX: the position of the lowest or highest
		// set bit, or the element count when there is none.
		llvm::Value* idx = nullptr;
		if (mostSignificant)
		{
			llvm::Value* lz = irb.CreateBinaryIntrinsic(llvm::Intrinsic::ctlz, res2, irb.getFalse());
			idx = irb.CreateSub(llvm::ConstantInt::get(maskTy, n - 1), lz);
		}
		else
		{
			// cttz with is_zero_poison false answers the bit width for zero,
			// which is exactly the "no match" answer the instruction wants --
			// but only for the least-significant direction.
			idx = irb.CreateBinaryIntrinsic(llvm::Intrinsic::cttz, res2, irb.getFalse());
		}
		idx = irb.CreateSelect(any, idx, llvm::ConstantInt::get(maskTy, n));
		storeRegister(X86_REG_ECX, irb.CreateZExt(idx, irb.getInt32Ty()), irb);
	}

	storeRegisters(
		irb,
		{{X86_REG_CF, any},
		 {X86_REG_ZF, irb.CreateICmpNE(z2, zeroMask)},
		 {X86_REG_SF, irb.CreateICmpNE(z1, zeroMask)},
		 {X86_REG_OF, irb.CreateTrunc(res2, irb.getInt1Ty())},
		 {X86_REG_AF, irb.getFalse()},
		 {X86_REG_PF, irb.getFalse()}});
}

/**
 * X86_INS_VBROADCASTSS/SD, X86_INS_VPBROADCASTB/W/D/Q -- splat one element
 * across the destination. ARM64 LD1R's analogue: one load, then every lane
 * is that value.
 *
 * avxWidth() cannot be used here. `vbroadcastss ymm0, xmm1` names a 256-bit
 * dest and a 128-bit source, and a memory source is the element width (4 or
 * 8 bytes), so the "all registers agree, memory matches" check returns 0
 * for every real encoding. Dest width comes from the destination operand
 * Capstone reports -- 128 for XMM, 256 for YMM. ZMM/EVEX-masked forms of
 * this broadcast stay unmapped here; `VPBROADCASTMB2Q`/`MW2D` go through
 * `translateAvxPmovNarrow`. VBROADCASTF128 (no VBROADCASTI128 ID in
 * this Capstone pin) is the 128-bit-block splat into both YMM lanes.
 *
 * A register source contributes only its low element; the rest of the XMM
 * is ignored, which is why the test uses a source whose high lanes differ.
 */
void Capstone2LlvmIrTranslatorX86_impl::translateAvxBroadcast(cs_insn* i, cs_x86* xi, llvm::IRBuilder<>& irb)
{
	if (xi->op_count != 2 || hasEvexModifier(xi))
	{
		translatePseudoAsmGeneric(i, xi, irb);
		return;
	}

	if (i->id == X86_INS_VBROADCASTF128
		|| i->id == X86_INS_VBROADCASTI32X4)
	{
		unsigned srcBits = 128;
		llvm::Value* block = loadVectorOp(xi->operands[1], irb, srcBits);
		if (block == nullptr)
		{
			translatePseudoAsmGeneric(i, xi, irb);
			return;
		}
		unsigned destBits = xi->operands[0].size * 8;
		if (destBits == 256)
		{
			storeVectorOp(xi->operands[0], concatLane128(block, block, irb), 256, irb);
			return;
		}
		if (destBits == 512)
		{
			llvm::Value* ymm = concatLane128(block, block, irb);
			storeVectorOp(
				xi->operands[0],
				irb.CreateOr(
					irb.CreateZExt(ymm, irb.getIntNTy(512)),
					irb.CreateShl(irb.CreateZExt(ymm, irb.getIntNTy(512)), ConstantInt::get(irb.getIntNTy(512), 256))),
				512,
				irb);
			return;
		}
		translatePseudoAsmGeneric(i, xi, irb);
		return;
	}
	if (i->id == X86_INS_VBROADCASTI64X4)
	{
		unsigned srcBits = 256;
		llvm::Value* block = loadVectorOp(xi->operands[1], irb, srcBits);
		if (block == nullptr)
		{
			translatePseudoAsmGeneric(i, xi, irb);
			return;
		}
		auto* i512 = irb.getIntNTy(512);
		llvm::Value* lo = irb.CreateZExt(irb.CreateZExtOrTrunc(block, irb.getIntNTy(256)), i512);
		storeVectorOp(xi->operands[0], irb.CreateOr(lo, irb.CreateShl(lo, ConstantInt::get(i512, 256))), 512, irb);
		return;
	}

	unsigned elemBits = 0;
	switch (i->id)
	{
	case X86_INS_VPBROADCASTB: elemBits = 8; break;
	case X86_INS_VPBROADCASTW: elemBits = 16; break;
	case X86_INS_VBROADCASTSS:
	case X86_INS_VPBROADCASTD: elemBits = 32; break;
	case X86_INS_VBROADCASTSD:
	case X86_INS_VPBROADCASTQ: elemBits = 64; break;
	default: translatePseudoAsmGeneric(i, xi, irb); return;
	}

	unsigned destBits = 0;
	if (xi->operands[0].type == X86_OP_REG)
	{
		destBits = vectorRegisterWidth(xi->operands[0].reg);
	}
	if (destBits != 128 && destBits != 256)
	{
		translatePseudoAsmGeneric(i, xi, irb);
		return;
	}
	if (destBits % elemBits != 0)
	{
		translatePseudoAsmGeneric(i, xi, irb);
		return;
	}

	auto* elemTy = irb.getIntNTy(elemBits);
	llvm::Value* elem = irb.CreateZExtOrTrunc(loadOp(xi->operands[1], irb), elemTy);

	unsigned n = destBits / elemBits;
	llvm::Value* splat = irb.CreateVectorSplat(n, elem);
	storeVectorOp(xi->operands[0], irb.CreateBitCast(splat, irb.getIntNTy(destBits)), destBits, irb);
}

static llvm::Value* perm2f128Select(llvm::Value* a, llvm::Value* b, unsigned ctrl, llvm::IRBuilder<>& irb)
{
	switch (ctrl & 3u)
	{
	case 0: return extractLane128(a, 0, irb);
	case 1: return extractLane128(a, 1, irb);
	case 2: return extractLane128(b, 0, irb);
	default: return extractLane128(b, 1, irb);
	}
}

/**
 * X86_INS_VINSERTF128, X86_INS_VEXTRACTF128, X86_INS_VPERM2F128
 * and the integer twins VINSERTI128 / VEXTRACTI128 / VPERM2I128.
 *
 * gcc -mavx emits these for 256-bit lane insert/extract/permute. They are
 * not a width-uniform AVX op -- vinsertf128 names a YMM destination and an
 * XMM source -- so avxWidth() returns 0 and they cannot share translateAvxMov.
 * The integer encodings use the same immediate layout over 128-bit halves.
 *
 * Capstone dump (64-bit):
 *   vinsertf128 ymm0, ymm1, xmm2, 1  -- 4 ops, dest ymm/32, src1 ymm/32,
 *     src2 xmm/16, imm8 lane (bit 0)
 *   vextractf128 xmm0, ymm1, 1       -- 3 ops, dest xmm/16, src ymm/32, imm8
 *   vperm2f128 ymm0, ymm1, ymm2, 0x20 -- 4 ops, all ymm/32, imm8 selects the
 *     source lane for each dest half (bits 1:0 and 5:4) and zeroes a half
 *     when bit 3 / bit 7 is set.
 */
void Capstone2LlvmIrTranslatorX86_impl::translateAvxF128Lane(cs_insn* i, cs_x86* xi, llvm::IRBuilder<>& irb)
{
	if (hasEvexModifier(xi))
	{
		translatePseudoAsmGeneric(i, xi, irb);
		return;
	}

	if (i->id == X86_INS_VINSERTF128 || i->id == X86_INS_VINSERTI128)
	{
		if (xi->op_count != 4 || xi->operands[3].type != X86_OP_IMM)
		{
			translatePseudoAsmGeneric(i, xi, irb);
			return;
		}

		unsigned destBits = xi->operands[0].type == X86_OP_REG
			? vectorRegisterWidth(xi->operands[0].reg)
			: 0;
		if (destBits != 256)
		{
			translatePseudoAsmGeneric(i, xi, irb);
			return;
		}

		unsigned src1Bits = 256;
		unsigned src2Bits = 128;
		llvm::Value* src1 = loadVectorOp(xi->operands[1], irb, src1Bits);
		llvm::Value* src2 = loadVectorOp(xi->operands[2], irb, src2Bits);
		if (src1 == nullptr || src2 == nullptr)
		{
			translatePseudoAsmGeneric(i, xi, irb);
			return;
		}

		llvm::Value* lo = extractLane128(src1, 0, irb);
		llvm::Value* hi = extractLane128(src1, 1, irb);
		llvm::Value* ins = irb.CreateZExtOrTrunc(src2, irb.getInt128Ty());
		if (xi->operands[3].imm & 1)
		{
			hi = ins;
		}
		else
		{
			lo = ins;
		}
		storeVectorOp(xi->operands[0], concatLane128(lo, hi, irb), 256, irb);
		return;
	}

	if (i->id == X86_INS_VEXTRACTF128 || i->id == X86_INS_VEXTRACTI128)
	{
		if (xi->op_count != 3 || xi->operands[2].type != X86_OP_IMM)
		{
			translatePseudoAsmGeneric(i, xi, irb);
			return;
		}

		unsigned srcBits = 256;
		llvm::Value* src = loadVectorOp(xi->operands[1], irb, srcBits);
		if (src == nullptr)
		{
			translatePseudoAsmGeneric(i, xi, irb);
			return;
		}

		unsigned lane = (xi->operands[2].imm & 1) ? 1u : 0u;
		storeVectorOp(xi->operands[0], extractLane128(src, lane, irb), 128, irb);
		return;
	}

	if (i->id == X86_INS_VPERM2F128 || i->id == X86_INS_VPERM2I128)
	{
		if (xi->op_count != 4 || xi->operands[3].type != X86_OP_IMM)
		{
			translatePseudoAsmGeneric(i, xi, irb);
			return;
		}

		unsigned bits = 256;
		llvm::Value* a = loadVectorOp(xi->operands[1], irb, bits);
		bits = 256;
		llvm::Value* b = loadVectorOp(xi->operands[2], irb, bits);
		if (a == nullptr || b == nullptr)
		{
			translatePseudoAsmGeneric(i, xi, irb);
			return;
		}

		uint64_t imm = static_cast<uint64_t>(xi->operands[3].imm) & 0xff;
		llvm::Value* zero = llvm::ConstantInt::get(irb.getInt128Ty(), 0);
		llvm::Value* lo = (imm & 0x08) ? zero : perm2f128Select(a, b, static_cast<unsigned>(imm), irb);
		llvm::Value* hi = (imm & 0x80) ? zero : perm2f128Select(a, b, static_cast<unsigned>(imm >> 4), irb);
		storeVectorOp(xi->operands[0], concatLane128(lo, hi, irb), 256, irb);
		return;
	}

	translatePseudoAsmGeneric(i, xi, irb);
}

//===========================================================================
// AES-NI / VAES / PCLMULQDQ / CRC32 / SHA — real llvm.x86.* intrinsics
//===========================================================================

static Value* bitcastIntToVecI64(Value* v, unsigned bits, IRBuilder<>& irb)
{
	auto* intTy = irb.getIntNTy(bits);
	if (v->getType() != intTy)
	{
		if (v->getType()->isIntegerTy())
		{
			v = irb.CreateZExtOrTrunc(v, intTy);
		}
		else
		{
			v = irb.CreateBitCast(v, intTy);
		}
	}
	return irb.CreateBitCast(v, FixedVectorType::get(irb.getInt64Ty(), bits / 64));
}

static Value* bitcastToV4I32(Value* v, IRBuilder<>& irb)
{
	return irb.CreateBitCast(toI128(v, irb), FixedVectorType::get(irb.getInt32Ty(), 4));
}

static Intrinsic::ID aesEncDecIntrinsic(unsigned id, unsigned bits)
{
	const bool enc = id == X86_INS_AESENC || id == X86_INS_VAESENC
			|| id == X86_INS_AESENCLAST || id == X86_INS_VAESENCLAST;
	const bool last = id == X86_INS_AESENCLAST || id == X86_INS_VAESENCLAST
			|| id == X86_INS_AESDECLAST || id == X86_INS_VAESDECLAST;

	if (bits == 128)
	{
		if (enc)
		{
			return last ? Intrinsic::x86_aesni_aesenclast : Intrinsic::x86_aesni_aesenc;
		}
		return last ? Intrinsic::x86_aesni_aesdeclast : Intrinsic::x86_aesni_aesdec;
	}
	if (bits == 256)
	{
		if (enc)
		{
			return last ? Intrinsic::x86_aesni_aesenclast_256 : Intrinsic::x86_aesni_aesenc_256;
		}
		return last ? Intrinsic::x86_aesni_aesdeclast_256 : Intrinsic::x86_aesni_aesdec_256;
	}
	if (bits == 512)
	{
		if (enc)
		{
			return last ? Intrinsic::x86_aesni_aesenclast_512 : Intrinsic::x86_aesni_aesenc_512;
		}
		return last ? Intrinsic::x86_aesni_aesdeclast_512 : Intrinsic::x86_aesni_aesdec_512;
	}
	return Intrinsic::not_intrinsic;
}

static bool isVaesEncDec(unsigned id)
{
	return id == X86_INS_VAESENC || id == X86_INS_VAESENCLAST
			|| id == X86_INS_VAESDEC || id == X86_INS_VAESDECLAST;
}

/**
 * AESENC / AESENCLAST / AESDEC / AESDECLAST and the VAES* VEX/EVEX forms.
 *
 * SSE: dest = AES*(dest, src) — two operands, dest is also state.
 * AVX: dest = AES*(src1, src2) — three operands, dest is not a source.
 * XMM is i128; the intrinsic takes <2 x i64> (or <4/8 x i64> at 256/512).
 */
void Capstone2LlvmIrTranslatorX86_impl::translateAesEncDec(
		cs_insn* i, cs_x86* xi, IRBuilder<>& irb)
{
	unsigned src1Idx = 0;
	unsigned src2Idx = 1;
	if (xi->op_count == 2)
	{
		src1Idx = 0;
		src2Idx = 1;
	}
	else if (xi->op_count == 3)
	{
		src1Idx = 1;
		src2Idx = 2;
	}
	else
	{
		translatePseudoAsmGeneric(i, xi, irb);
		return;
	}

	const bool vex = isVaesEncDec(i->id);
	unsigned bits = 128;
	Value* state = nullptr;
	Value* roundKey = nullptr;

	if (vex)
	{
		bits = avxWidth(xi);
		if (bits != 128 && bits != 256 && bits != 512)
		{
			translatePseudoAsmGeneric(i, xi, irb);
			return;
		}
		unsigned b1 = bits;
		unsigned b2 = bits;
		state = loadVectorOp(xi->operands[src1Idx], irb, b1);
		roundKey = loadVectorOp(xi->operands[src2Idx], irb, b2);
		if (state == nullptr || roundKey == nullptr)
		{
			translatePseudoAsmGeneric(i, xi, irb);
			return;
		}
	}
	else
	{
		state = loadOp(xi->operands[src1Idx], irb);
		roundKey = loadOp(xi->operands[src2Idx], irb);
		if (state == nullptr || roundKey == nullptr)
		{
			translatePseudoAsmGeneric(i, xi, irb);
			return;
		}
	}

	Intrinsic::ID iid = aesEncDecIntrinsic(i->id, bits);
	if (iid == Intrinsic::not_intrinsic)
	{
		translatePseudoAsmGeneric(i, xi, irb);
		return;
	}

	auto* fn = Intrinsic::getOrInsertDeclaration(_module, iid);
	Value* res = irb.CreateCall(
			fn,
			{bitcastIntToVecI64(state, bits, irb), bitcastIntToVecI64(roundKey, bits, irb)});
	Value* out = irb.CreateBitCast(res, irb.getIntNTy(bits));
	if (vex)
	{
		storeVectorOp(xi->operands[0], out, bits, irb);
	}
	else
	{
		storeOp(xi->operands[0], out, irb, eOpConv::NOTHING);
	}
}

/**
 * AESIMC / VAESIMC — dest = AESIMC(src). 128-bit only.
 */
void Capstone2LlvmIrTranslatorX86_impl::translateAesImc(
		cs_insn* i, cs_x86* xi, IRBuilder<>& irb)
{
	EXPECT_IS_BINARY(i, xi, irb);

	const bool vex = i->id == X86_INS_VAESIMC;
	unsigned bits = 128;
	Value* src = nullptr;
	if (vex)
	{
		src = loadVectorOp(xi->operands[1], irb, bits);
		if (src == nullptr || bits != 128)
		{
			translatePseudoAsmGeneric(i, xi, irb);
			return;
		}
	}
	else
	{
		src = loadOp(xi->operands[1], irb);
		if (src == nullptr)
		{
			translatePseudoAsmGeneric(i, xi, irb);
			return;
		}
	}

	auto* fn = Intrinsic::getOrInsertDeclaration(_module, Intrinsic::x86_aesni_aesimc);
	Value* res = irb.CreateCall(fn, {bitcastIntToVecI64(src, 128, irb)});
	Value* out = irb.CreateBitCast(res, irb.getInt128Ty());
	if (vex)
	{
		storeVectorOp(xi->operands[0], out, 128, irb);
	}
	else
	{
		storeOp(xi->operands[0], out, irb, eOpConv::NOTHING);
	}
}

/**
 * AESKEYGENASSIST / VAESKEYGENASSIST — dest = AESKEYGENASSIST(src, imm8).
 */
void Capstone2LlvmIrTranslatorX86_impl::translateAesKeygenAssist(
		cs_insn* i, cs_x86* xi, IRBuilder<>& irb)
{
	EXPECT_IS_TERNARY(i, xi, irb);
	if (xi->operands[2].type != X86_OP_IMM)
	{
		translatePseudoAsmGeneric(i, xi, irb);
		return;
	}

	const bool vex = i->id == X86_INS_VAESKEYGENASSIST;
	unsigned bits = 128;
	Value* src = nullptr;
	if (vex)
	{
		src = loadVectorOp(xi->operands[1], irb, bits);
		if (src == nullptr || bits != 128)
		{
			translatePseudoAsmGeneric(i, xi, irb);
			return;
		}
	}
	else
	{
		src = loadOp(xi->operands[1], irb);
		if (src == nullptr)
		{
			translatePseudoAsmGeneric(i, xi, irb);
			return;
		}
	}

	auto* fn = Intrinsic::getOrInsertDeclaration(_module, Intrinsic::x86_aesni_aeskeygenassist);
	Value* imm = ConstantInt::get(irb.getInt8Ty(), static_cast<uint8_t>(xi->operands[2].imm));
	Value* res = irb.CreateCall(fn, {bitcastIntToVecI64(src, 128, irb), imm});
	Value* out = irb.CreateBitCast(res, irb.getInt128Ty());
	if (vex)
	{
		storeVectorOp(xi->operands[0], out, 128, irb);
	}
	else
	{
		storeOp(xi->operands[0], out, irb, eOpConv::NOTHING);
	}
}

/**
 * PCLMULQDQ / VPCLMULQDQ — dest = PCLMULQDQ(src1, src2, imm8).
 * SSE is dest/src1, src2, imm. VEX is dest, src1, src2, imm.
 */
void Capstone2LlvmIrTranslatorX86_impl::translatePclmulqdq(
		cs_insn* i, cs_x86* xi, IRBuilder<>& irb)
{
	unsigned src1Idx = 0;
	unsigned src2Idx = 1;
	unsigned immIdx = 2;
	if (xi->op_count == 3 && xi->operands[2].type == X86_OP_IMM)
	{
		src1Idx = 0;
		src2Idx = 1;
		immIdx = 2;
	}
	else if (xi->op_count == 4 && xi->operands[3].type == X86_OP_IMM)
	{
		src1Idx = 1;
		src2Idx = 2;
		immIdx = 3;
	}
	else
	{
		translatePseudoAsmGeneric(i, xi, irb);
		return;
	}

	const bool vex = i->id == X86_INS_VPCLMULQDQ;
	unsigned bits = 128;
	Value* a = nullptr;
	Value* b = nullptr;

	if (vex)
	{
		bits = avxWidth(xi);
		if (bits != 128 && bits != 256 && bits != 512)
		{
			translatePseudoAsmGeneric(i, xi, irb);
			return;
		}
		unsigned b1 = bits;
		unsigned b2 = bits;
		a = loadVectorOp(xi->operands[src1Idx], irb, b1);
		b = loadVectorOp(xi->operands[src2Idx], irb, b2);
		if (a == nullptr || b == nullptr)
		{
			translatePseudoAsmGeneric(i, xi, irb);
			return;
		}
	}
	else
	{
		a = loadOp(xi->operands[src1Idx], irb);
		b = loadOp(xi->operands[src2Idx], irb);
		if (a == nullptr || b == nullptr)
		{
			translatePseudoAsmGeneric(i, xi, irb);
			return;
		}
	}

	Intrinsic::ID iid = Intrinsic::not_intrinsic;
	if (bits == 128)
	{
		iid = Intrinsic::x86_pclmulqdq;
	}
	else if (bits == 256)
	{
		iid = Intrinsic::x86_pclmulqdq_256;
	}
	else if (bits == 512)
	{
		iid = Intrinsic::x86_pclmulqdq_512;
	}
	if (iid == Intrinsic::not_intrinsic)
	{
		translatePseudoAsmGeneric(i, xi, irb);
		return;
	}

	auto* fn = Intrinsic::getOrInsertDeclaration(_module, iid);
	Value* imm = ConstantInt::get(irb.getInt8Ty(), static_cast<uint8_t>(xi->operands[immIdx].imm));
	Value* res = irb.CreateCall(
			fn,
			{bitcastIntToVecI64(a, bits, irb), bitcastIntToVecI64(b, bits, irb), imm});
	Value* out = irb.CreateBitCast(res, irb.getIntNTy(bits));
	if (vex)
	{
		storeVectorOp(xi->operands[0], out, bits, irb);
	}
	else
	{
		storeOp(xi->operands[0], out, irb, eOpConv::NOTHING);
	}
}

/**
 * CRC32 r32/r64, r/m8|16|32|64 — llvm.x86.sse42.crc32.{32.8,32.16,32.32,64.64}.
 * LLVM has no 64.8 form; CRC32 r64, r/m8 falls back rather than invent one.
 */
void Capstone2LlvmIrTranslatorX86_impl::translateCrc32(
		cs_insn* i, cs_x86* xi, IRBuilder<>& irb)
{
	EXPECT_IS_BINARY(i, xi, irb);

	Value* crc = loadOp(xi->operands[0], irb);
	Value* data = loadOp(xi->operands[1], irb);
	if (crc == nullptr || data == nullptr || !crc->getType()->isIntegerTy()
			|| !data->getType()->isIntegerTy())
	{
		translatePseudoAsmGeneric(i, xi, irb);
		return;
	}

	unsigned crcBits = crc->getType()->getIntegerBitWidth();
	unsigned dataBits = data->getType()->getIntegerBitWidth();
	// Capstone's operand size is the architectural width; prefer it when the
	// loaded value is a parent register that we then need to truncate.
	if (xi->operands[0].size)
	{
		crcBits = static_cast<unsigned>(xi->operands[0].size) * 8;
		crc = irb.CreateZExtOrTrunc(crc, irb.getIntNTy(crcBits));
	}
	if (xi->operands[1].size)
	{
		dataBits = static_cast<unsigned>(xi->operands[1].size) * 8;
		data = irb.CreateZExtOrTrunc(data, irb.getIntNTy(dataBits));
	}

	Intrinsic::ID iid = Intrinsic::not_intrinsic;
	if (crcBits == 32 && dataBits == 8)
	{
		iid = Intrinsic::x86_sse42_crc32_32_8;
	}
	else if (crcBits == 32 && dataBits == 16)
	{
		iid = Intrinsic::x86_sse42_crc32_32_16;
	}
	else if (crcBits == 32 && dataBits == 32)
	{
		iid = Intrinsic::x86_sse42_crc32_32_32;
	}
	else if (crcBits == 64 && dataBits == 64)
	{
		iid = Intrinsic::x86_sse42_crc32_64_64;
	}
	if (iid == Intrinsic::not_intrinsic)
	{
		translatePseudoAsmGeneric(i, xi, irb);
		return;
	}

	auto* fn = Intrinsic::getOrInsertDeclaration(_module, iid);
	Value* res = irb.CreateCall(fn, {crc, data});
	storeOp(xi->operands[0], res, irb, eOpConv::ZEXT_TRUNC_OR_BITCAST);
}

/**
 * SHA1MSG1/MSG2/NEXTE/RNDS4 and SHA256MSG1/MSG2/RNDS2.
 * XMM is i128; the SHA intrinsics take <4 x i32>.
 * SHA256RNDS2 implicitly reads XMM0 as the third operand.
 */
void Capstone2LlvmIrTranslatorX86_impl::translateSha(
		cs_insn* i, cs_x86* xi, IRBuilder<>& irb)
{
	auto storeXmm = [&](Value* vec) {
		storeOp(xi->operands[0], irb.CreateBitCast(vec, irb.getInt128Ty()), irb, eOpConv::NOTHING);
	};

	switch (i->id)
	{
	case X86_INS_SHA1MSG1:
	case X86_INS_SHA1MSG2:
	case X86_INS_SHA1NEXTE:
	case X86_INS_SHA256MSG1:
	case X86_INS_SHA256MSG2:
	{
		EXPECT_IS_BINARY(i, xi, irb);
		Value* a = loadOp(xi->operands[0], irb);
		Value* b = loadOp(xi->operands[1], irb);
		if (a == nullptr || b == nullptr)
		{
			translatePseudoAsmGeneric(i, xi, irb);
			return;
		}
		Intrinsic::ID iid = Intrinsic::x86_sha1msg1;
		if (i->id == X86_INS_SHA1MSG2)
		{
			iid = Intrinsic::x86_sha1msg2;
		}
		else if (i->id == X86_INS_SHA1NEXTE)
		{
			iid = Intrinsic::x86_sha1nexte;
		}
		else if (i->id == X86_INS_SHA256MSG1)
		{
			iid = Intrinsic::x86_sha256msg1;
		}
		else if (i->id == X86_INS_SHA256MSG2)
		{
			iid = Intrinsic::x86_sha256msg2;
		}
		auto* fn = Intrinsic::getOrInsertDeclaration(_module, iid);
		storeXmm(irb.CreateCall(fn, {bitcastToV4I32(a, irb), bitcastToV4I32(b, irb)}));
		return;
	}
	case X86_INS_SHA1RNDS4:
	{
		EXPECT_IS_TERNARY(i, xi, irb);
		if (xi->operands[2].type != X86_OP_IMM)
		{
			translatePseudoAsmGeneric(i, xi, irb);
			return;
		}
		Value* a = loadOp(xi->operands[0], irb);
		Value* b = loadOp(xi->operands[1], irb);
		if (a == nullptr || b == nullptr)
		{
			translatePseudoAsmGeneric(i, xi, irb);
			return;
		}
		auto* fn = Intrinsic::getOrInsertDeclaration(_module, Intrinsic::x86_sha1rnds4);
		Value* imm = ConstantInt::get(irb.getInt8Ty(), static_cast<uint8_t>(xi->operands[2].imm));
		storeXmm(irb.CreateCall(fn, {bitcastToV4I32(a, irb), bitcastToV4I32(b, irb), imm}));
		return;
	}
	case X86_INS_SHA256RNDS2:
	{
		if (xi->op_count != 2 && xi->op_count != 3)
		{
			translatePseudoAsmGeneric(i, xi, irb);
			return;
		}
		Value* a = loadOp(xi->operands[0], irb);
		Value* b = loadOp(xi->operands[1], irb);
		Value* c = (xi->op_count == 3)
				? loadOp(xi->operands[2], irb)
				: loadRegister(X86_REG_XMM0, irb);
		if (a == nullptr || b == nullptr || c == nullptr)
		{
			translatePseudoAsmGeneric(i, xi, irb);
			return;
		}
		auto* fn = Intrinsic::getOrInsertDeclaration(_module, Intrinsic::x86_sha256rnds2);
		storeXmm(irb.CreateCall(
				fn,
				{bitcastToV4I32(a, irb), bitcastToV4I32(b, irb), bitcastToV4I32(c, irb)}));
		return;
	}
	default:
		translatePseudoAsmGeneric(i, xi, irb);
		return;
	}
}

/**
 * RCPPS / RCPSS / RSQRTPS / RSQRTSS — 12-bit reciprocal / reciprocal-sqrt
 * estimates. llvm.x86.sse.{rcp,rsqrt}.{ps,ss} take <4 x float>.
 * Scalar forms update only lane 0; the destination's other lanes stay.
 */
void Capstone2LlvmIrTranslatorX86_impl::translateSseRcpRsqrt(
		cs_insn* i, cs_x86* xi, IRBuilder<>& irb)
{
	const bool isRsqrt = i->id == X86_INS_RSQRTPS || i->id == X86_INS_RSQRTSS
		|| i->id == X86_INS_VRSQRTPS || i->id == X86_INS_VRSQRTSS
		|| i->id == X86_INS_VRSQRT14PS || i->id == X86_INS_VRSQRT14PD
		|| i->id == X86_INS_VRSQRT14SS || i->id == X86_INS_VRSQRT14SD;
	const bool isScalar = i->id == X86_INS_RCPSS || i->id == X86_INS_RSQRTSS
		|| i->id == X86_INS_VRCPSS || i->id == X86_INS_VRSQRTSS
		|| i->id == X86_INS_VRCP14SS || i->id == X86_INS_VRCP14SD
		|| i->id == X86_INS_VRSQRT14SS || i->id == X86_INS_VRSQRT14SD;
	const bool approx14 = i->id == X86_INS_VRCP14PS || i->id == X86_INS_VRCP14PD
		|| i->id == X86_INS_VRCP14SS || i->id == X86_INS_VRCP14SD
		|| i->id == X86_INS_VRSQRT14PS || i->id == X86_INS_VRSQRT14PD
		|| i->id == X86_INS_VRSQRT14SS || i->id == X86_INS_VRSQRT14SD;
	bool vex = (i->id == X86_INS_VRCPPS || i->id == X86_INS_VRCPSS
		|| i->id == X86_INS_VRSQRTPS || i->id == X86_INS_VRSQRTSS || approx14);
	if (approx14)
	{
		bool isDouble = i->id == X86_INS_VRCP14PD || i->id == X86_INS_VRCP14SD
			|| i->id == X86_INS_VRSQRT14PD || i->id == X86_INS_VRSQRT14SD;
		unsigned destBits = xi->operands[0].size * 8;
		if (destBits != 128 && destBits != 256 && destBits != 512)
		{
			destBits = 128;
		}
		unsigned srcOp = isScalar ? (xi->op_count >= 3 ? 2u : 1u) : 1u;
		unsigned keepOp = isScalar ? 1u : 0u;
		unsigned loadBits = destBits;
		Value* src = loadVectorOp(xi->operands[srcOp], irb, loadBits);
		if (src == nullptr)
		{
			src = toI128(loadOp(xi->operands[srcOp], irb), irb);
			destBits = 128;
		}
		Type* elemTy = isDouble ? static_cast<Type*>(irb.getDoubleTy()) : static_cast<Type*>(irb.getFloatTy());
		unsigned elemBits = isDouble ? 64u : 32u;
		unsigned n = destBits / elemBits;
		auto* vecTy = FixedVectorType::get(elemTy, n);
		Value* v = irb.CreateBitCast(irb.CreateZExtOrTrunc(src, irb.getIntNTy(destBits)), vecTy);
		Value* one = ConstantFP::get(elemTy, 1.0);
		if (isRsqrt)
		{
			v = irb.CreateFDiv(irb.CreateVectorSplat(n, one), irb.CreateUnaryIntrinsic(Intrinsic::sqrt, v));
		}
		else
		{
			v = irb.CreateFDiv(irb.CreateVectorSplat(n, one), v);
		}
		if (isScalar && xi->op_count >= 3)
		{
			unsigned keepBits = destBits;
			Value* keep = loadVectorOp(xi->operands[keepOp], irb, keepBits);
			if (keep != nullptr)
			{
				Value* kv = irb.CreateBitCast(irb.CreateZExtOrTrunc(keep, irb.getIntNTy(destBits)), vecTy);
				v = irb.CreateInsertElement(kv, irb.CreateExtractElement(v, (uint64_t)0), (uint64_t)0);
			}
		}
		storeVectorOp(xi->operands[0], irb.CreateBitCast(v, irb.getIntNTy(destBits)), destBits, irb);
		return;
	}
	Intrinsic::ID iid = isRsqrt
			? (isScalar ? Intrinsic::x86_sse_rsqrt_ss : Intrinsic::x86_sse_rsqrt_ps)
			: (isScalar ? Intrinsic::x86_sse_rcp_ss : Intrinsic::x86_sse_rcp_ps);

	auto* vec4f = vecType(irb.getFloatTy(), 4);
	if (vex && isScalar)
	{
		if (xi->op_count != 3)
		{
			translatePseudoAsmGeneric(i, xi, irb);
			return;
		}
		Value* dst = irb.CreateBitCast(toI128(loadOp(xi->operands[1], irb), irb), vec4f);
		Value* src = irb.CreateBitCast(toI128(loadOp(xi->operands[2], irb), irb), vec4f);
		src = irb.CreateInsertElement(dst, irb.CreateExtractElement(src, (uint64_t)0), (uint64_t)0);
		auto* fn = Intrinsic::getOrInsertDeclaration(_module, iid);
		storeVectorOp(xi->operands[0], irb.CreateBitCast(irb.CreateCall(fn, {src}), irb.getInt128Ty()), 128, irb);
		return;
	}
	if (vex)
	{
		EXPECT_IS_BINARY(i, xi, irb);
		if (xi->operands[0].size != 16)
		{
			translatePseudoAsmGeneric(i, xi, irb);
			return;
		}
		Value* src = irb.CreateBitCast(toI128(loadOp(xi->operands[1], irb), irb), vec4f);
		auto* fn = Intrinsic::getOrInsertDeclaration(_module, iid);
		storeVectorOp(xi->operands[0], irb.CreateBitCast(irb.CreateCall(fn, {src}), irb.getInt128Ty()), 128, irb);
		return;
	}

	EXPECT_IS_BINARY(i, xi, irb);
	Value* src = irb.CreateBitCast(toI128(loadOp(xi->operands[1], irb), irb), vec4f);
	if (isScalar)
	{
		Value* dst = irb.CreateBitCast(toI128(loadOp(xi->operands[0], irb), irb), vec4f);
		src = irb.CreateInsertElement(dst, irb.CreateExtractElement(src, (uint64_t)0), (uint64_t)0);
	}

	auto* fn = Intrinsic::getOrInsertDeclaration(_module, iid);
	Value* res = irb.CreateCall(fn, {src});
	storeOp(xi->operands[0], irb.CreateBitCast(res, irb.getInt128Ty()), irb, eOpConv::NOTHING);
}

/**
 * CVTPD2DQ — two doubles → two int32s in the low half, high 64 bits zeroed,
 * rounded to nearest-even. CVTTPD2DQ truncates toward zero.
 */
void Capstone2LlvmIrTranslatorX86_impl::translateCvtPd2Dq(
		cs_insn* i, cs_x86* xi, IRBuilder<>& irb)
{
	EXPECT_IS_BINARY(i, xi, irb);

	bool vex = (i->id == X86_INS_VCVTPD2DQ || i->id == X86_INS_VCVTTPD2DQ);
	bool trunc = (i->id == X86_INS_CVTTPD2DQ || i->id == X86_INS_VCVTTPD2DQ);
	unsigned srcBits = xi->operands[1].size * 8;
	if (srcBits == 0)
	{
		srcBits = 128;
	}
	unsigned n = srcBits / 64;
	unsigned destBits = vex ? 128 : 128;
	if (vex && xi->operands[0].size == 16 && srcBits == 256)
	{
		// vcvtpd2dq xmm, ymm: four doubles become four int32s.
		destBits = 128;
		n = 4;
	}
	else if (vex && srcBits != 128 && srcBits != 256)
	{
		translatePseudoAsmGeneric(i, xi, irb);
		return;
	}

	unsigned loadBits = srcBits;
	Value* raw = vex ? loadVectorOp(xi->operands[1], irb, loadBits) : loadOp(xi->operands[1], irb);
	if (raw == nullptr)
	{
		translatePseudoAsmGeneric(i, xi, irb);
		return;
	}
	auto* vecD = vecType(irb.getDoubleTy(), n);
	Value* v = irb.CreateBitCast(irb.CreateZExtOrTrunc(vex ? raw : toI128(raw, irb), irb.getIntNTy(n * 64)), vecD);
	Value* packed = ConstantInt::get(irb.getIntNTy(n * 32), 0);
	auto* packedTy = packed->getType();
	for (unsigned lane = 0; lane < n; ++lane)
	{
		Value* e = irb.CreateExtractElement(v, (uint64_t)lane);
		if (!trunc)
		{
			e = irb.CreateUnaryIntrinsic(Intrinsic::roundeven, e);
		}
		Value* conv = generateFpToSiDefined(e, irb.getInt32Ty(), irb);
		Value* w = irb.CreateZExt(conv, packedTy);
		if (lane != 0)
		{
			w = irb.CreateShl(w, ConstantInt::get(packedTy, 32 * lane));
		}
		packed = irb.CreateOr(packed, w);
	}
	if (vex)
	{
		storeVectorOp(xi->operands[0], irb.CreateZExt(packed, irb.getInt128Ty()), 128, irb);
	}
	else
	{
		storeOp(xi->operands[0], irb.CreateZExt(packed, irb.getInt128Ty()), irb, eOpConv::NOTHING);
	}
}

/**
 * VIA PadLock: named helpers in the xcore.thread.* style, not __asm_*.
 * Implicit ESI/EDI/EAX (and EBX/ECX/EDX for XCRYPT*).
 */
void Capstone2LlvmIrTranslatorX86_impl::translatePadlock(
		cs_insn* i, cs_x86* xi, IRBuilder<>& irb)
{
	const char* name = nullptr;
	bool xcrypt = false;
	switch (i->id)
	{
	case X86_INS_XSHA1: name = "x86.padlock.xsha1"; break;
	case X86_INS_XSHA256: name = "x86.padlock.xsha256"; break;
	case X86_INS_XSTORE: name = "x86.padlock.xstore"; break;
	case X86_INS_XCRYPTCBC: name = "x86.padlock.xcrypt.cbc"; xcrypt = true; break;
	case X86_INS_XCRYPTCFB: name = "x86.padlock.xcrypt.cfb"; xcrypt = true; break;
	case X86_INS_XCRYPTCTR: name = "x86.padlock.xcrypt.ctr"; xcrypt = true; break;
	case X86_INS_XCRYPTECB: name = "x86.padlock.xcrypt.ecb"; xcrypt = true; break;
	case X86_INS_XCRYPTOFB: name = "x86.padlock.xcrypt.ofb"; xcrypt = true; break;
	default:
		translatePseudoAsmGeneric(i, xi, irb);
		return;
	}

	auto esi = getParentRegister(X86_REG_ESI);
	auto edi = getParentRegister(X86_REG_EDI);
	auto eax = getParentRegister(X86_REG_EAX);
	Value* vEsi = loadRegister(esi, irb);
	Value* vEdi = loadRegister(edi, irb);
	Value* vEax = loadRegister(eax, irb);

	if (xcrypt)
	{
		auto ebx = getParentRegister(X86_REG_EBX);
		auto ecx = getParentRegister(X86_REG_ECX);
		auto edx = getParentRegister(X86_REG_EDX);
		Value* vEbx = loadRegister(ebx, irb);
		Value* vEcx = loadRegister(ecx, irb);
		Value* vEdx = loadRegister(edx, irb);
		auto* retTy = llvm::StructType::create(llvm::ArrayRef<llvm::Type*>{
				vEsi->getType(), vEdi->getType(), vEcx->getType()});
		auto* fnc = getPseudoAsmFunction(
				i,
				retTy,
				llvm::ArrayRef<llvm::Type*>{
						vEsi->getType(),
						vEdi->getType(),
						vEax->getType(),
						vEbx->getType(),
						vEcx->getType(),
						vEdx->getType()},
				name);
		auto* c = irb.CreateCall(
				fnc,
				llvm::ArrayRef<llvm::Value*>{vEsi, vEdi, vEax, vEbx, vEcx, vEdx});
		storeRegister(esi, irb.CreateExtractValue(c, {0}), irb);
		storeRegister(edi, irb.CreateExtractValue(c, {1}), irb);
		storeRegister(ecx, irb.CreateExtractValue(c, {2}), irb);
		return;
	}

	auto* retTy = llvm::StructType::create(llvm::ArrayRef<llvm::Type*>{
			vEsi->getType(), vEdi->getType(), vEax->getType()});
	auto* fnc = getPseudoAsmFunction(
			i,
			retTy,
			llvm::ArrayRef<llvm::Type*>{vEsi->getType(), vEdi->getType(), vEax->getType()},
			name);
	auto* c = irb.CreateCall(fnc, llvm::ArrayRef<llvm::Value*>{vEsi, vEdi, vEax});
	storeRegister(esi, irb.CreateExtractValue(c, {0}), irb);
	storeRegister(edi, irb.CreateExtractValue(c, {1}), irb);
	storeRegister(eax, irb.CreateExtractValue(c, {2}), irb);
}

/**
 * MULX r32/r64, r32/r64, r/m — unsigned multiply of implicit EDX/RDX by src.
 * High half → dest1, low half → dest2. Does not write flags.
 */
void Capstone2LlvmIrTranslatorX86_impl::translateMulx(
		cs_insn* i, cs_x86* xi, IRBuilder<>& irb)
{
	EXPECT_IS_TERNARY(i, xi, irb);

	unsigned bits = xi->operands[0].size ? xi->operands[0].size * 8u : 32u;
	if (bits != 32 && bits != 64)
	{
		translatePseudoAsmGeneric(i, xi, irb);
		return;
	}

	auto* halfT = irb.getIntNTy(bits);
	auto* mulT = irb.getIntNTy(bits * 2);
	Value* src1 = loadRegister(bits == 64 ? X86_REG_RDX : X86_REG_EDX, irb);
	Value* src2 = loadOp(xi->operands[2], irb);
	if (src1 == nullptr || src2 == nullptr)
	{
		translatePseudoAsmGeneric(i, xi, irb);
		return;
	}
	src1 = irb.CreateZExt(irb.CreateZExtOrTrunc(src1, halfT), mulT);
	src2 = irb.CreateZExt(irb.CreateZExtOrTrunc(src2, halfT), mulT);
	Value* prod = irb.CreateMul(src1, src2);
	Value* low = irb.CreateTrunc(prod, halfT);
	Value* high = irb.CreateTrunc(irb.CreateLShr(prod, bits), halfT);
	storeOp(xi->operands[0], high, irb, eOpConv::ZEXT_TRUNC_OR_BITCAST);
	storeOp(xi->operands[1], low, irb, eOpConv::ZEXT_TRUNC_OR_BITCAST);
}

static Value* bitcastIntToTypedVec(Value* v, Type* vecTy, unsigned bits, IRBuilder<>& irb)
{
	auto* intTy = irb.getIntNTy(bits);
	if (v->getType() != intTy)
	{
		if (v->getType()->isIntegerTy())
		{
			v = irb.CreateZExtOrTrunc(v, intTy);
		}
		else
		{
			v = irb.CreateBitCast(v, intTy);
		}
	}
	return irb.CreateBitCast(v, vecTy);
}

/**
 * DPPS/DPPD and VDPPS/VDPPD — llvm.x86.sse41.dpps / dppd, ymm VDPPS uses
 * llvm.x86.avx.dp.ps.256. SSE is dest,src,imm; AVX is dest,src1,src2,imm.
 */
void Capstone2LlvmIrTranslatorX86_impl::translateSseDp(
		cs_insn* i, cs_x86* xi, IRBuilder<>& irb)
{
	const bool vex = i->id == X86_INS_VDPPD || i->id == X86_INS_VDPPS;
	const bool isDppd = i->id == X86_INS_DPPD || i->id == X86_INS_VDPPD;
	if (vex)
	{
		EXPECT_IS_QUATERNARY(i, xi, irb);
	}
	else
	{
		EXPECT_IS_TERNARY(i, xi, irb);
	}

	unsigned immIdx = vex ? 3u : 2u;
	if (xi->operands[immIdx].type != X86_OP_IMM)
	{
		translatePseudoAsmGeneric(i, xi, irb);
		return;
	}

	unsigned bits = 128;
	Value* a = nullptr;
	Value* b = nullptr;
	if (vex)
	{
		a = loadVectorOp(xi->operands[1], irb, bits);
		unsigned bBits = bits;
		b = loadVectorOp(xi->operands[2], irb, bBits);
		if (a == nullptr || b == nullptr || bits != bBits)
		{
			translatePseudoAsmGeneric(i, xi, irb);
			return;
		}
	}
	else
	{
		a = loadOp(xi->operands[0], irb);
		b = loadOp(xi->operands[1], irb);
		if (a == nullptr || b == nullptr)
		{
			translatePseudoAsmGeneric(i, xi, irb);
			return;
		}
		bits = 128;
	}

	if (isDppd && bits != 128)
	{
		translatePseudoAsmGeneric(i, xi, irb);
		return;
	}

	Intrinsic::ID iid = Intrinsic::not_intrinsic;
	Type* vecTy = nullptr;
	if (isDppd)
	{
		iid = Intrinsic::x86_sse41_dppd;
		vecTy = vecType(irb.getDoubleTy(), 2);
	}
	else if (bits == 256)
	{
		iid = Intrinsic::x86_avx_dp_ps_256;
		vecTy = vecType(irb.getFloatTy(), 8);
	}
	else
	{
		iid = Intrinsic::x86_sse41_dpps;
		vecTy = vecType(irb.getFloatTy(), 4);
	}

	Value* va = bitcastIntToTypedVec(a, vecTy, bits, irb);
	Value* vb = bitcastIntToTypedVec(b, vecTy, bits, irb);
	Value* imm = ConstantInt::get(irb.getInt8Ty(), static_cast<uint8_t>(xi->operands[immIdx].imm));
	auto* fn = Intrinsic::getOrInsertDeclaration(_module, iid);
	Value* res = irb.CreateCall(fn, {va, vb, imm});
	Value* out = irb.CreateBitCast(res, irb.getIntNTy(bits));
	if (vex)
	{
		storeVectorOp(xi->operands[0], out, bits, irb);
	}
	else
	{
		storeOp(xi->operands[0], out, irb, eOpConv::NOTHING);
	}
}

/**
 * MPSADBW / VMPSADBW — llvm.x86.sse41.mpsadbw, ymm uses llvm.x86.avx2.mpsadbw.
 */
void Capstone2LlvmIrTranslatorX86_impl::translateSseMpsadbw(
		cs_insn* i, cs_x86* xi, IRBuilder<>& irb)
{
	const bool vex = i->id == X86_INS_VMPSADBW;
	if (vex)
	{
		EXPECT_IS_QUATERNARY(i, xi, irb);
	}
	else
	{
		EXPECT_IS_TERNARY(i, xi, irb);
	}

	unsigned immIdx = vex ? 3u : 2u;
	if (xi->operands[immIdx].type != X86_OP_IMM)
	{
		translatePseudoAsmGeneric(i, xi, irb);
		return;
	}

	unsigned bits = 128;
	Value* a = nullptr;
	Value* b = nullptr;
	if (vex)
	{
		a = loadVectorOp(xi->operands[1], irb, bits);
		unsigned bBits = bits;
		b = loadVectorOp(xi->operands[2], irb, bBits);
		if (a == nullptr || b == nullptr || bits != bBits)
		{
			translatePseudoAsmGeneric(i, xi, irb);
			return;
		}
	}
	else
	{
		a = loadOp(xi->operands[0], irb);
		b = loadOp(xi->operands[1], irb);
		if (a == nullptr || b == nullptr)
		{
			translatePseudoAsmGeneric(i, xi, irb);
			return;
		}
		bits = 128;
	}

	Intrinsic::ID iid = (bits == 256)
			? Intrinsic::x86_avx2_mpsadbw
			: Intrinsic::x86_sse41_mpsadbw;
	auto* srcTy = vecType(irb.getInt8Ty(), bits / 8);
	Value* va = bitcastIntToTypedVec(a, srcTy, bits, irb);
	Value* vb = bitcastIntToTypedVec(b, srcTy, bits, irb);
	Value* imm = ConstantInt::get(irb.getInt8Ty(), static_cast<uint8_t>(xi->operands[immIdx].imm));
	auto* fn = Intrinsic::getOrInsertDeclaration(_module, iid);
	Value* res = irb.CreateCall(fn, {va, vb, imm});
	Value* out = irb.CreateBitCast(res, irb.getIntNTy(bits));
	if (vex)
	{
		storeVectorOp(xi->operands[0], out, bits, irb);
	}
	else
	{
		storeOp(xi->operands[0], out, irb, eOpConv::NOTHING);
	}
}

/**
 * LDMXCSR / STMXCSR — llvm.x86.sse.ldmxcsr / stmxcsr on the m32 address.
 */
void Capstone2LlvmIrTranslatorX86_impl::translateMxcsr(
		cs_insn* i, cs_x86* xi, IRBuilder<>& irb)
{
	EXPECT_IS_UNARY(i, xi, irb);
	if (xi->operands[0].type != X86_OP_MEM)
	{
		translatePseudoAsmGeneric(i, xi, irb);
		return;
	}

	Value* addr = loadOp(xi->operands[0], irb, nullptr, /*lea=*/true);
	if (addr == nullptr)
	{
		translatePseudoAsmGeneric(i, xi, irb);
		return;
	}
	Value* ptr = irb.CreateIntToPtr(
			addr,
			PointerType::get(irb.getInt8Ty(), getAddrSpace(xi->operands[0].mem.segment)));
	Intrinsic::ID iid = (i->id == X86_INS_LDMXCSR || i->id == X86_INS_VLDMXCSR)
			? Intrinsic::x86_sse_ldmxcsr
			: Intrinsic::x86_sse_stmxcsr;
	auto* fn = Intrinsic::getOrInsertDeclaration(_module, iid);
	irb.CreateCall(fn, {ptr});
}

static llvm::Value* permuteSelectLane(
	llvm::Value* src,
	llvm::Value* sel,
	unsigned n,
	llvm::IRBuilder<>& irb)
{
	llvm::Value* elem = irb.CreateExtractElement(src, (uint64_t)0);
	for (unsigned k = 1; k < n; ++k)
	{
		llvm::Value* take = irb.CreateICmpEQ(sel, llvm::ConstantInt::get(sel->getType(), k));
		elem = irb.CreateSelect(take, irb.CreateExtractElement(src, k), elem);
	}
	return elem;
}

static llvm::Value* permilpdI128(llvm::Value* src, unsigned ctrl, llvm::IRBuilder<>& irb)
{
	auto* vecTy = llvm::FixedVectorType::get(irb.getInt64Ty(), 2);
	llvm::Value* v = irb.CreateBitCast(toI128(src, irb), vecTy);
	int mask[2] = {static_cast<int>(ctrl & 1u), static_cast<int>((ctrl >> 1) & 1u)};
	return irb.CreateBitCast(irb.CreateShuffleVector(v, v, mask), irb.getInt128Ty());
}

/**
 * VPERMILPS/PD (imm and variable, in-lane), VPERMD/VPERMPS (AVX2 dword
 * cross-lane), VPERMQ/VPERMPD (imm8 64-bit ymm permute). XOP VPERMIL2*
 * stays unmapped.
 */
void Capstone2LlvmIrTranslatorX86_impl::translateAvxPermute(cs_insn* i, cs_x86* xi, llvm::IRBuilder<>& irb)
{
	if (xi->op_count < 3 || hasEvexModifier(xi))
	{
		translatePseudoAsmGeneric(i, xi, irb);
		return;
	}

	unsigned destBits = xi->operands[0].size * 8;
	if (destBits != 128 && destBits != 256)
	{
		translatePseudoAsmGeneric(i, xi, irb);
		return;
	}

	bool immForm = xi->operands[xi->op_count - 1].type == X86_OP_IMM;
	unsigned srcBits = destBits;
	llvm::Value* src = loadVectorOp(xi->operands[1], irb, srcBits);
	if (src == nullptr)
	{
		translatePseudoAsmGeneric(i, xi, irb);
		return;
	}

	llvm::Value* out = nullptr;
	switch (i->id)
	{
	case X86_INS_VPERMILPS:
		if (immForm)
		{
			uint8_t ctrl = static_cast<uint8_t>(xi->operands[2].imm);
			if (destBits == 128)
			{
				out = pshufdI128(src, ctrl, irb);
			}
			else
			{
				out = concatLane128(
					pshufdI128(extractLane128(src, 0, irb), ctrl, irb),
					pshufdI128(extractLane128(src, 1, irb), ctrl, irb),
					irb);
			}
		}
		else
		{
			unsigned n = destBits / 32;
			auto* vecTy = vecType(irb.getInt32Ty(), n);
			llvm::Value* vs = irb.CreateBitCast(irb.CreateZExtOrTrunc(src, irb.getIntNTy(destBits)), vecTy);
			unsigned idxBits = destBits;
			llvm::Value* idx = loadVectorOp(xi->operands[2], irb, idxBits);
			if (idx == nullptr)
			{
				translatePseudoAsmGeneric(i, xi, irb);
				return;
			}
			llvm::Value* vi = irb.CreateBitCast(irb.CreateZExtOrTrunc(idx, irb.getIntNTy(destBits)), vecTy);
			out = llvm::UndefValue::get(vecTy);
			for (unsigned lane = 0; lane < n; ++lane)
			{
				llvm::Value* sel = irb.CreateAnd(
					irb.CreateExtractElement(vi, lane),
					llvm::ConstantInt::get(irb.getInt32Ty(), 3));
				sel = irb.CreateAdd(sel, llvm::ConstantInt::get(irb.getInt32Ty(), lane & ~3u));
				out = irb.CreateInsertElement(out, permuteSelectLane(vs, sel, n, irb), lane);
			}
			out = irb.CreateBitCast(out, irb.getIntNTy(destBits));
		}
		break;
	case X86_INS_VPERMILPD:
		if (immForm)
		{
			unsigned ctrl = static_cast<unsigned>(xi->operands[2].imm);
			if (destBits == 128)
			{
				out = permilpdI128(src, ctrl, irb);
			}
			else
			{
				out = concatLane128(
					permilpdI128(extractLane128(src, 0, irb), ctrl, irb),
					permilpdI128(extractLane128(src, 1, irb), ctrl >> 2, irb),
					irb);
			}
		}
		else
		{
			unsigned n = destBits / 64;
			auto* vecTy = vecType(irb.getInt64Ty(), n);
			llvm::Value* vs = irb.CreateBitCast(irb.CreateZExtOrTrunc(src, irb.getIntNTy(destBits)), vecTy);
			unsigned idxBits = destBits;
			llvm::Value* idx = loadVectorOp(xi->operands[2], irb, idxBits);
			if (idx == nullptr)
			{
				translatePseudoAsmGeneric(i, xi, irb);
				return;
			}
			llvm::Value* vi = irb.CreateBitCast(irb.CreateZExtOrTrunc(idx, irb.getIntNTy(destBits)), vecTy);
			out = llvm::UndefValue::get(vecTy);
			for (unsigned lane = 0; lane < n; ++lane)
			{
				llvm::Value* sel = irb.CreateAnd(
					irb.CreateLShr(irb.CreateExtractElement(vi, lane), llvm::ConstantInt::get(irb.getInt64Ty(), 1)),
					llvm::ConstantInt::get(irb.getInt64Ty(), 1));
				sel = irb.CreateAdd(sel, llvm::ConstantInt::get(irb.getInt64Ty(), lane & ~1u));
				out = irb.CreateInsertElement(out, permuteSelectLane(vs, sel, n, irb), lane);
			}
			out = irb.CreateBitCast(out, irb.getIntNTy(destBits));
		}
		break;
	case X86_INS_VPERMD:
	case X86_INS_VPERMPS:
		if (destBits != 256 || immForm)
		{
			translatePseudoAsmGeneric(i, xi, irb);
			return;
		}
		{
			// vpermd ymm1, ymm2, ymm3: DEST[i] = SRC2[SRC1[i] & 7]
			unsigned dataBits = 256;
			llvm::Value* data = loadVectorOp(xi->operands[2], irb, dataBits);
			if (data == nullptr)
			{
				translatePseudoAsmGeneric(i, xi, irb);
				return;
			}
			unsigned n = 8;
			auto* vecTy = vecType(irb.getInt32Ty(), n);
			llvm::Value* vs = irb.CreateBitCast(irb.CreateZExtOrTrunc(data, irb.getIntNTy(256)), vecTy);
			llvm::Value* vi = irb.CreateBitCast(irb.CreateZExtOrTrunc(src, irb.getIntNTy(256)), vecTy);
			out = llvm::UndefValue::get(vecTy);
			for (unsigned lane = 0; lane < n; ++lane)
			{
				llvm::Value* sel = irb.CreateAnd(
					irb.CreateExtractElement(vi, lane),
					llvm::ConstantInt::get(irb.getInt32Ty(), 7));
				out = irb.CreateInsertElement(out, permuteSelectLane(vs, sel, n, irb), lane);
			}
			out = irb.CreateBitCast(out, irb.getIntNTy(256));
		}
		break;
	case X86_INS_VPERMQ:
	case X86_INS_VPERMPD:
		if (destBits != 256 || !immForm)
		{
			translatePseudoAsmGeneric(i, xi, irb);
			return;
		}
		{
			auto* vecTy = vecType(irb.getInt64Ty(), 4);
			llvm::Value* vs = irb.CreateBitCast(irb.CreateZExtOrTrunc(src, irb.getIntNTy(256)), vecTy);
			unsigned ctrl = static_cast<unsigned>(xi->operands[2].imm);
			int mask[4] = {
				static_cast<int>(ctrl & 3u),
				static_cast<int>((ctrl >> 2) & 3u),
				static_cast<int>((ctrl >> 4) & 3u),
				static_cast<int>((ctrl >> 6) & 3u)};
			out = irb.CreateBitCast(irb.CreateShuffleVector(vs, vs, mask), irb.getIntNTy(256));
		}
		break;
	default:
		translatePseudoAsmGeneric(i, xi, irb);
		return;
	}

	storeVectorOp(xi->operands[0], out, destBits, irb);
}

/**
 * AVX2 per-lane variable shifts. Count is the matching element of the
 * second vector; a count >= element width yields 0 (logical) or sign-fill
 * (arithmetic).
 */
void Capstone2LlvmIrTranslatorX86_impl::translateAvxVariableShift(cs_insn* i, cs_x86* xi, llvm::IRBuilder<>& irb)
{
	unsigned elemBits = 0;
	bool arith = false;
	bool left = false;
	switch (i->id)
	{
	case X86_INS_VPSLLVD: elemBits = 32; left = true; break;
	case X86_INS_VPSLLVQ: elemBits = 64; left = true; break;
	case X86_INS_VPSRLVD: elemBits = 32; break;
	case X86_INS_VPSRLVQ: elemBits = 64; break;
	case X86_INS_VPSRAVD: elemBits = 32; arith = true; break;
	case X86_INS_VPSRAVQ: elemBits = 64; arith = true; break;
	case X86_INS_VPSLLVW: elemBits = 16; left = true; break;
	case X86_INS_VPSRLVW: elemBits = 16; break;
	case X86_INS_VPSRAVW: elemBits = 16; arith = true; break;
	default:
		translatePseudoAsmGeneric(i, xi, irb);
		return;
	}

	if (xi->op_count != 3 || hasEvexModifier(xi))
	{
		translatePseudoAsmGeneric(i, xi, irb);
		return;
	}

	unsigned destBits = xi->operands[0].size * 8;
	if ((destBits != 128 && destBits != 256) || destBits % elemBits != 0)
	{
		translatePseudoAsmGeneric(i, xi, irb);
		return;
	}

	unsigned srcBits = destBits;
	unsigned cntBits = destBits;
	llvm::Value* src = loadVectorOp(xi->operands[1], irb, srcBits);
	llvm::Value* cnt = loadVectorOp(xi->operands[2], irb, cntBits);
	if (src == nullptr || cnt == nullptr)
	{
		translatePseudoAsmGeneric(i, xi, irb);
		return;
	}

	unsigned n = destBits / elemBits;
	auto* elemTy = irb.getIntNTy(elemBits);
	auto* vecTy = vecType(elemTy, n);
	auto* intTy = irb.getIntNTy(destBits);
	llvm::Value* vs = irb.CreateBitCast(irb.CreateZExtOrTrunc(src, intTy), vecTy);
	llvm::Value* vc = irb.CreateBitCast(irb.CreateZExtOrTrunc(cnt, intTy), vecTy);
	llvm::Value* width = llvm::ConstantInt::get(elemTy, elemBits);
	llvm::Value* maxShift = llvm::ConstantInt::get(elemTy, elemBits - 1);
	llvm::Value* zero = llvm::Constant::getNullValue(elemTy);
	llvm::Value* res = llvm::UndefValue::get(vecTy);
	for (unsigned lane = 0; lane < n; ++lane)
	{
		llvm::Value* a = irb.CreateExtractElement(vs, lane);
		llvm::Value* c = irb.CreateExtractElement(vc, lane);
		llvm::Value* tooBig = irb.CreateICmpUGE(c, width);
		llvm::Value* cSafe = irb.CreateSelect(tooBig, maxShift, c);
		llvm::Value* shifted = nullptr;
		if (left)
		{
			shifted = irb.CreateSelect(tooBig, zero, irb.CreateShl(a, cSafe));
		}
		else if (arith)
		{
			shifted = irb.CreateAShr(a, cSafe);
		}
		else
		{
			shifted = irb.CreateSelect(tooBig, zero, irb.CreateLShr(a, cSafe));
		}
		res = irb.CreateInsertElement(res, shifted, lane);
	}
	storeVectorOp(xi->operands[0], irb.CreateBitCast(res, intTy), destBits, irb);
}

/**
 * AVX2 VSIB gather (not VGATHERPF*). Dest lanes whose mask MSB is clear
 * are preserved; every mask lane is then zeroed.
 */
void Capstone2LlvmIrTranslatorX86_impl::translateAvxGather(cs_insn* i, cs_x86* xi, llvm::IRBuilder<>& irb)
{
	if (xi->op_count != 3 || xi->operands[1].type != X86_OP_MEM || hasEvexModifier(xi))
	{
		translatePseudoAsmGeneric(i, xi, irb);
		return;
	}

	unsigned dataBits = 0;
	unsigned idxBits = 0;
	switch (i->id)
	{
	case X86_INS_VGATHERDPS:
	case X86_INS_VPGATHERDD: dataBits = 32; idxBits = 32; break;
	case X86_INS_VGATHERDPD:
	case X86_INS_VPGATHERDQ: dataBits = 64; idxBits = 32; break;
	case X86_INS_VGATHERQPS:
	case X86_INS_VPGATHERQD: dataBits = 32; idxBits = 64; break;
	case X86_INS_VGATHERQPD:
	case X86_INS_VPGATHERQQ: dataBits = 64; idxBits = 64; break;
	default:
		translatePseudoAsmGeneric(i, xi, irb);
		return;
	}

	auto& mem = xi->operands[1].mem;
	if (mem.index == X86_REG_INVALID)
	{
		translatePseudoAsmGeneric(i, xi, irb);
		return;
	}

	unsigned destBits = xi->operands[0].size * 8;
	if (destBits != 128 && destBits != 256)
	{
		translatePseudoAsmGeneric(i, xi, irb);
		return;
	}

	unsigned n = (idxBits == 64 && dataBits == 32) ? (destBits / 64) : (destBits / dataBits);
	unsigned destLoadBits = destBits;
	llvm::Value* dest = loadVectorOp(xi->operands[0], irb, destLoadBits);
	unsigned maskBits = destBits;
	llvm::Value* mask = loadVectorOp(xi->operands[2], irb, maskBits);
	if (dest == nullptr || mask == nullptr)
	{
		translatePseudoAsmGeneric(i, xi, irb);
		return;
	}

	cs_x86_op idxOp{};
	idxOp.type = X86_OP_REG;
	idxOp.reg = mem.index;
	idxOp.size = static_cast<uint8_t>(vectorRegisterWidth(mem.index) / 8);
	unsigned idxRegBits = idxOp.size * 8;
	llvm::Value* idxRaw = loadVectorOp(idxOp, irb, idxRegBits);
	if (idxRaw == nullptr || idxRegBits < n * idxBits)
	{
		translatePseudoAsmGeneric(i, xi, irb);
		return;
	}

	unsigned destLanes = destBits / dataBits;
	auto* dataTy = irb.getIntNTy(dataBits);
	auto* destVecTy = vecType(dataTy, destLanes);
	auto* idxElemTy = irb.getIntNTy(idxBits);
	auto* idxVecTy = vecType(idxElemTy, idxRegBits / idxBits);
	auto* destIntTy = irb.getIntNTy(destBits);
	llvm::Value* destV = irb.CreateBitCast(irb.CreateZExtOrTrunc(dest, destIntTy), destVecTy);
	llvm::Value* maskV = irb.CreateBitCast(irb.CreateZExtOrTrunc(mask, destIntTy), destVecTy);
	llvm::Value* idxV = irb.CreateBitCast(irb.CreateZExtOrTrunc(idxRaw, irb.getIntNTy(idxRegBits)), idxVecTy);

	llvm::Type* addrTy = getDefaultType();
	llvm::Value* base = (mem.base != X86_REG_INVALID)
		? irb.CreateSExtOrTrunc(loadRegister(mem.base, irb), addrTy)
		: llvm::ConstantInt::get(addrTy, 0);
	llvm::Value* disp = llvm::ConstantInt::get(addrTy, static_cast<uint64_t>(mem.disp), true);
	llvm::Value* scale = llvm::ConstantInt::get(addrTy, mem.scale ? mem.scale : 1);
	unsigned as = getAddrSpace(mem.segment);

	llvm::Value* res = destV;
	for (unsigned lane = 0; lane < n; ++lane)
	{
		llvm::Value* idx = irb.CreateSExtOrTrunc(irb.CreateExtractElement(idxV, lane), addrTy);
		llvm::Value* ea = irb.CreateAdd(irb.CreateAdd(base, disp), irb.CreateMul(idx, scale));
		llvm::Value* loaded = loadIntPtr(irb, ea, dataTy, as);
		llvm::Value* take = irb.CreateICmpSLT(
			irb.CreateExtractElement(maskV, lane),
			llvm::Constant::getNullValue(dataTy));
		res = irb.CreateInsertElement(
			res,
			irb.CreateSelect(take, loaded, irb.CreateExtractElement(destV, lane)),
			lane);
	}
	if (n < destLanes)
	{
		for (unsigned lane = n; lane < destLanes; ++lane)
		{
			res = irb.CreateInsertElement(res, llvm::Constant::getNullValue(dataTy), lane);
		}
	}

	storeVectorOp(xi->operands[0], irb.CreateBitCast(res, destIntTy), destBits, irb);
	storeVectorOp(xi->operands[2], llvm::ConstantInt::get(destIntTy, 0), destBits, irb);
}

static llvm::Value* f16BitsToF32Bits(llvm::Value* h16, llvm::IRBuilder<>& irb)
{
	llvm::Value* h = irb.CreateZExt(h16, irb.getInt32Ty());
	llvm::Value* sign = irb.CreateShl(
		irb.CreateAnd(irb.CreateLShr(h, llvm::ConstantInt::get(irb.getInt32Ty(), 15)), llvm::ConstantInt::get(irb.getInt32Ty(), 1)),
		llvm::ConstantInt::get(irb.getInt32Ty(), 31));
	llvm::Value* exp = irb.CreateAnd(irb.CreateLShr(h, llvm::ConstantInt::get(irb.getInt32Ty(), 10)), llvm::ConstantInt::get(irb.getInt32Ty(), 0x1f));
	llvm::Value* frac = irb.CreateAnd(h, llvm::ConstantInt::get(irb.getInt32Ty(), 0x3ff));
	llvm::Value* isZero = irb.CreateICmpEQ(exp, llvm::ConstantInt::get(irb.getInt32Ty(), 0));
	llvm::Value* isAllOnes = irb.CreateICmpEQ(exp, llvm::ConstantInt::get(irb.getInt32Ty(), 31));
	llvm::Value* norm = irb.CreateOr(
		irb.CreateShl(irb.CreateAdd(exp, llvm::ConstantInt::get(irb.getInt32Ty(), 112)), llvm::ConstantInt::get(irb.getInt32Ty(), 23)),
		irb.CreateShl(frac, llvm::ConstantInt::get(irb.getInt32Ty(), 13)));
	llvm::Value* special = irb.CreateOr(
		irb.CreateShl(llvm::ConstantInt::get(irb.getInt32Ty(), 255), llvm::ConstantInt::get(irb.getInt32Ty(), 23)),
		irb.CreateShl(frac, llvm::ConstantInt::get(irb.getInt32Ty(), 13)));
	llvm::Value* mag = irb.CreateSelect(isZero, llvm::ConstantInt::get(irb.getInt32Ty(), 0), irb.CreateSelect(isAllOnes, special, norm));
	return irb.CreateOr(sign, mag);
}

static llvm::Value* f32BitsToF16Bits(llvm::Value* f32, llvm::IRBuilder<>& irb)
{
	auto* i32 = irb.getInt32Ty();
	llvm::Value* sign = irb.CreateLShr(irb.CreateAnd(f32, llvm::ConstantInt::get(i32, 0x80000000u)), llvm::ConstantInt::get(i32, 16));
	llvm::Value* exp = irb.CreateAnd(irb.CreateLShr(f32, llvm::ConstantInt::get(i32, 23)), llvm::ConstantInt::get(i32, 0xff));
	llvm::Value* frac = irb.CreateAnd(f32, llvm::ConstantInt::get(i32, 0x7fffff));
	llvm::Value* e16 = irb.CreateSub(exp, llvm::ConstantInt::get(i32, 112));
	llvm::Value* mag = irb.CreateOr(irb.CreateShl(e16, llvm::ConstantInt::get(i32, 10)), irb.CreateLShr(frac, llvm::ConstantInt::get(i32, 13)));
	llvm::Value* tooSmall = irb.CreateICmpULT(exp, llvm::ConstantInt::get(i32, 113));
	llvm::Value* tooBig = irb.CreateICmpUGT(exp, llvm::ConstantInt::get(i32, 142));
	llvm::Value* isInfNan = irb.CreateICmpEQ(exp, llvm::ConstantInt::get(i32, 255));
	mag = irb.CreateSelect(tooSmall, llvm::ConstantInt::get(i32, 0), mag);
	mag = irb.CreateSelect(tooBig, llvm::ConstantInt::get(i32, 0x7c00), mag);
	mag = irb.CreateSelect(
		isInfNan,
		irb.CreateOr(llvm::ConstantInt::get(i32, 0x7c00), irb.CreateLShr(frac, llvm::ConstantInt::get(i32, 13))),
		mag);
	return irb.CreateTrunc(irb.CreateOr(sign, mag), irb.getInt16Ty());
}

/**
 * F16C VCVTPH2PS / VCVTPS2PH. This Capstone pin has no non-V CVTPH2PS /
 * CVTPS2PH IDs. Rounding control in the store form is ignored; conversion
 * is bit-exact for finite values that fit in both formats.
 */
void Capstone2LlvmIrTranslatorX86_impl::translateF16c(cs_insn* i, cs_x86* xi, llvm::IRBuilder<>& irb)
{
	if (hasEvexModifier(xi))
	{
		translatePseudoAsmGeneric(i, xi, irb);
		return;
	}

	if (i->id == X86_INS_VCVTPH2PS)
	{
		if (xi->op_count != 2)
		{
			translatePseudoAsmGeneric(i, xi, irb);
			return;
		}
		unsigned destBits = xi->operands[0].size * 8;
		if (destBits != 128 && destBits != 256)
		{
			translatePseudoAsmGeneric(i, xi, irb);
			return;
		}
		unsigned n = destBits / 32;
		unsigned srcBits = n * 16;
		llvm::Value* raw = nullptr;
		if (xi->operands[1].type == X86_OP_MEM)
		{
			raw = loadOp(xi->operands[1], irb, irb.getIntNTy(srcBits));
		}
		else
		{
			unsigned loadBits = (xi->operands[1].size * 8) >= 128 ? 128u : srcBits;
			if (loadBits < srcBits)
			{
				loadBits = 128;
			}
			raw = loadVectorOp(xi->operands[1], irb, loadBits);
		}
		if (raw == nullptr)
		{
			translatePseudoAsmGeneric(i, xi, irb);
			return;
		}
		auto* hVec = vecType(irb.getInt16Ty(), n);
		llvm::Value* hv = irb.CreateBitCast(irb.CreateZExtOrTrunc(raw, irb.getIntNTy(srcBits)), hVec);
		auto* fVec = vecType(irb.getInt32Ty(), n);
		llvm::Value* fv = llvm::UndefValue::get(fVec);
		for (unsigned lane = 0; lane < n; ++lane)
		{
			fv = irb.CreateInsertElement(fv, f16BitsToF32Bits(irb.CreateExtractElement(hv, lane), irb), lane);
		}
		storeVectorOp(xi->operands[0], irb.CreateBitCast(fv, irb.getIntNTy(destBits)), destBits, irb);
		return;
	}

	if (i->id == X86_INS_VCVTPS2PH)
	{
		if (xi->op_count != 3 || xi->operands[2].type != X86_OP_IMM)
		{
			translatePseudoAsmGeneric(i, xi, irb);
			return;
		}
		unsigned srcBits = xi->operands[1].size * 8;
		if (srcBits != 128 && srcBits != 256)
		{
			translatePseudoAsmGeneric(i, xi, irb);
			return;
		}
		unsigned n = srcBits / 32;
		unsigned outBits = n * 16;
		unsigned loadBits = srcBits;
		llvm::Value* raw = loadVectorOp(xi->operands[1], irb, loadBits);
		if (raw == nullptr)
		{
			translatePseudoAsmGeneric(i, xi, irb);
			return;
		}
		auto* fVec = vecType(irb.getInt32Ty(), n);
		llvm::Value* fv = irb.CreateBitCast(irb.CreateZExtOrTrunc(raw, irb.getIntNTy(srcBits)), fVec);
		auto* hVec = vecType(irb.getInt16Ty(), n);
		llvm::Value* hv = llvm::UndefValue::get(hVec);
		for (unsigned lane = 0; lane < n; ++lane)
		{
			hv = irb.CreateInsertElement(hv, f32BitsToF16Bits(irb.CreateExtractElement(fv, lane), irb), lane);
		}
		llvm::Value* packed = irb.CreateBitCast(hv, irb.getIntNTy(outBits));
		if (xi->operands[0].type == X86_OP_MEM)
		{
			storeOp(xi->operands[0], packed, irb, eOpConv::NOTHING);
		}
		else
		{
			storeVectorOp(xi->operands[0], irb.CreateZExt(packed, irb.getInt128Ty()), 128, irb);
		}
		return;
	}

	translatePseudoAsmGeneric(i, xi, irb);
}

static unsigned sseCmpPredFromMnemonic(const char* mnem)
{
	if (mnem == nullptr)
	{
		return 0;
	}
	if (mnem[0] == 'v' || mnem[0] == 'V')
	{
		++mnem;
	}
	if ((mnem[0] == 'c' || mnem[0] == 'C') && (mnem[1] == 'm' || mnem[1] == 'M')
		&& (mnem[2] == 'p' || mnem[2] == 'P'))
	{
		mnem += 3;
	}
	std::string s(mnem);
	auto strip = [&](const char* suf) {
		size_t n = std::strlen(suf);
		if (s.size() >= n && s.compare(s.size() - n, n, suf) == 0)
		{
			s.resize(s.size() - n);
			return true;
		}
		return false;
	};
	strip("ps") || strip("pd") || strip("ss") || strip("sd");
	static const struct
	{
		const char* name;
		unsigned pred;
	} table[] = {
		{"eq_oq", 0},	 {"lt_os", 1},	   {"le_os", 2},	 {"unord_q", 3}, {"neq_uq", 4},
		{"nlt_us", 5},	 {"nle_us", 6},	   {"ord_q", 7},	 {"eq_uq", 8},	 {"nge_us", 9},
		{"ngt_us", 10},	 {"false_oq", 11}, {"neq_oq", 12},	 {"ge_os", 13},	 {"gt_os", 14},
		{"true_uq", 15}, {"eq_os", 16},	   {"lt_oq", 17},	 {"le_oq", 18},	 {"unord_s", 19},
		{"neq_us", 20},	 {"nlt_uq", 21},   {"nle_uq", 22},	 {"ord_s", 23},	 {"eq_us", 24},
		{"nge_uq", 25},	 {"ngt_uq", 26},   {"false_os", 27}, {"neq_os", 28}, {"ge_oq", 29},
		{"gt_oq", 30},	 {"true_us", 31},  {"unord", 3},	 {"false", 11},	 {"true", 15},
		{"neq", 4},		 {"nlt", 5},	   {"nle", 6},		 {"nge", 9},	 {"ngt", 10},
		{"ord", 7},		 {"eq", 0},		   {"lt", 1},		 {"le", 2},		 {"ge", 13},
		{"gt", 14},
	};
	for (const auto& e : table)
	{
		if (s == e.name)
		{
			return e.pred;
		}
	}
	return 0;
}

static llvm::CmpInst::Predicate sseFpCmpPred(unsigned imm)
{
	switch (imm & 31u)
	{
	case 0:
	case 16: return llvm::CmpInst::FCMP_OEQ;
	case 1:
	case 17: return llvm::CmpInst::FCMP_OLT;
	case 2:
	case 18: return llvm::CmpInst::FCMP_OLE;
	case 3:
	case 19: return llvm::CmpInst::FCMP_UNO;
	case 4:
	case 20: return llvm::CmpInst::FCMP_UNE;
	case 5:
	case 21: return llvm::CmpInst::FCMP_UGE;
	case 6:
	case 22: return llvm::CmpInst::FCMP_UGT;
	case 7:
	case 23: return llvm::CmpInst::FCMP_ORD;
	case 8:
	case 24: return llvm::CmpInst::FCMP_UEQ;
	case 9:
	case 25: return llvm::CmpInst::FCMP_ULT;
	case 10:
	case 26: return llvm::CmpInst::FCMP_ULE;
	case 11:
	case 27: return llvm::CmpInst::FCMP_FALSE;
	case 12:
	case 28: return llvm::CmpInst::FCMP_ONE;
	case 13:
	case 29: return llvm::CmpInst::FCMP_OGE;
	case 14:
	case 30: return llvm::CmpInst::FCMP_OGT;
	default: return llvm::CmpInst::FCMP_TRUE;
	}
}

/**
 * CMPPS/CMPPD/CMPSS and the VEX/EVEX twins. Result lanes are all-1s or all-0s.
 * A k-register destination writes a predicate mask instead.
 */
void Capstone2LlvmIrTranslatorX86_impl::translateSseCmpP(cs_insn* i, cs_x86* xi, IRBuilder<>& irb)
{
	bool isDouble = false;
	bool isScalar = false;
	bool vex = false;
	switch (i->id)
	{
	case X86_INS_VCMPPS: vex = true; break;
	case X86_INS_CMPPS: break;
	case X86_INS_VCMPPD: vex = true; isDouble = true; break;
	case X86_INS_CMPPD: isDouble = true; break;
	case X86_INS_VCMPSS: vex = true; isScalar = true; break;
	case X86_INS_CMPSS: isScalar = true; break;
	case X86_INS_VCMPSD: vex = true; isDouble = true; isScalar = true; break;
	case X86_INS_CMPSD: isDouble = true; isScalar = true; break;
	case X86_INS_VCMP:
		vex = true;
		{
			std::string m(i->mnemonic ? i->mnemonic : "");
			isDouble = m.size() >= 2 && (m.compare(m.size() - 2, 2, "pd") == 0 || m.compare(m.size() - 2, 2, "sd") == 0);
			isScalar = m.size() >= 2 && (m.compare(m.size() - 2, 2, "ss") == 0 || m.compare(m.size() - 2, 2, "sd") == 0);
		}
		break;
	default: translatePseudoAsmGeneric(i, xi, irb); return;
	}

	// Capstone's ID for the 0F C2 family is not trustworthy once the
	// predicate is folded into the mnemonic (cmpeqps / vcmpeqss / ...).
	// The suffix is the width: ps/ss = f32, pd/sd = f64.
	if (i->mnemonic != nullptr && i->mnemonic[0] != '\0')
	{
		const char* m = i->mnemonic;
		if (m[0] == 'v' || m[0] == 'V')
		{
			++m;
		}
		size_t nlen = std::strlen(m);
		if (nlen >= 2)
		{
			char a = m[nlen - 2];
			char b = m[nlen - 1];
			if (a == 'p' || a == 'P' || a == 's' || a == 'S')
			{
				isDouble = (b == 'd' || b == 'D');
				isScalar = (a == 's' || a == 'S');
			}
		}
	}

	// Capstone often folds imm8 into the mnemonic (cmpeqps) and drops the
	// immediate operand. Prefer an explicit IMM when present.
	unsigned srcA = 0;
	unsigned srcB = 1;
	unsigned imm = sseCmpPredFromMnemonic(i->mnemonic);
	if (xi->op_count >= 3 && xi->operands[xi->op_count - 1].type == X86_OP_IMM)
	{
		imm = static_cast<unsigned>(xi->operands[xi->op_count - 1].imm) & 31u;
		if (xi->op_count >= 4)
		{
			vex = true;
			srcA = 1;
			srcB = 2;
		}
	}
	else if (xi->op_count >= 3)
	{
		vex = true;
		srcA = 1;
		srcB = 2;
	}
	else if (xi->op_count != 2)
	{
		translatePseudoAsmGeneric(i, xi, irb);
		return;
	}
	auto pred = sseFpCmpPred(imm);

	bool destIsK = xi->operands[0].type == X86_OP_REG
		&& xi->operands[0].reg >= X86_REG_K0 && xi->operands[0].reg <= X86_REG_K7;

	unsigned destBits = isScalar ? 128u : (xi->operands[0].size * 8);
	if (!destIsK && destBits != 128 && destBits != 256 && destBits != 512)
	{
		destBits = 128;
	}
	if (destIsK)
	{
		destBits = xi->operands[1].size * 8;
		if (destBits != 128 && destBits != 256 && destBits != 512)
		{
			destBits = 128;
		}
	}

	Type* elemTy = isDouble ? static_cast<Type*>(irb.getDoubleTy()) : static_cast<Type*>(irb.getFloatTy());
	unsigned elemBits = isDouble ? 64u : 32u;
	unsigned n = destBits / elemBits;
	unsigned loadA = destBits;
	unsigned loadB = destBits;
	Value* aRaw = loadVectorOp(xi->operands[srcA], irb, loadA);
	Value* bRaw = loadVectorOp(xi->operands[srcB], irb, loadB);
	if (aRaw == nullptr || bRaw == nullptr)
	{
		aRaw = toI128(loadOp(xi->operands[srcA], irb), irb);
		bRaw = toI128(loadOp(xi->operands[srcB], irb), irb);
		destBits = 128;
		n = destBits / elemBits;
	}

	// Per-lane scalar fcmp + integer deposit. Vector fcmp/sext/insertelement
	// of <4 x float> / <N x i1> is not reliable in the LLVM interpreter used
	// by these tests (CMPPS stored 0; CMPSD clobbered the upper qword).
	auto* intTy = irb.getIntNTy(destBits);
	auto* elemInt = irb.getIntNTy(elemBits);
	aRaw = irb.CreateZExtOrTrunc(aRaw, intTy);
	bRaw = irb.CreateZExtOrTrunc(bRaw, intTy);
	Value* out = isScalar ? aRaw : llvm::ConstantInt::get(intTy, 0);
	Value* kbits = destIsK ? llvm::ConstantInt::get(irb.getIntNTy(n < 8 ? 8 : n), 0) : nullptr;
	Value* onesLane = llvm::Constant::getAllOnesValue(elemInt);
	Value* laneMask0 = irb.CreateZExt(onesLane, intTy);
	for (unsigned lane = 0; lane < n; ++lane)
	{
		if (isScalar && lane != 0)
		{
			break;
		}
		Value* shift = llvm::ConstantInt::get(intTy, static_cast<uint64_t>(lane) * elemBits);
		Value* ea = irb.CreateBitCast(irb.CreateTrunc(irb.CreateLShr(aRaw, shift), elemInt), elemTy);
		Value* eb = irb.CreateBitCast(irb.CreateTrunc(irb.CreateLShr(bRaw, shift), elemInt), elemTy);
		Value* c = irb.CreateFCmp(pred, ea, eb);
		if (destIsK)
		{
			kbits = irb.CreateOr(
				kbits, irb.CreateShl(irb.CreateZExt(c, kbits->getType()), (uint64_t)lane));
		}
		else
		{
			Value* laneVal = irb.CreateShl(irb.CreateZExt(irb.CreateSExt(c, elemInt), intTy), shift);
			Value* laneMask = irb.CreateShl(laneMask0, shift);
			out = irb.CreateOr(irb.CreateAnd(out, irb.CreateNot(laneMask)), laneVal);
		}
	}
	if (destIsK)
	{
		storeMaskOp(xi->operands[0], kbits, n, irb);
		return;
	}
	if (vex)
	{
		storeVectorOp(xi->operands[0], out, destBits, irb);
	}
	else
	{
		storeOp(xi->operands[0], irb.CreateZExtOrTrunc(out, irb.getInt128Ty()), irb, eOpConv::NOTHING);
	}
}

void Capstone2LlvmIrTranslatorX86_impl::translateMaskMovdqu(cs_insn* i, cs_x86* xi, IRBuilder<>& irb)
{
	if (xi->op_count != 2)
	{
		translatePseudoAsmGeneric(i, xi, irb);
		return;
	}
	Value* data = toI128(loadOp(xi->operands[0], irb), irb);
	Value* mask = toI128(loadOp(xi->operands[1], irb), irb);
	auto* vecTy = vecType(irb.getInt8Ty(), 16);
	Value* dv = irb.CreateBitCast(data, vecTy);
	Value* mv = irb.CreateBitCast(mask, vecTy);
	uint32_t di = (_origBasicMode == CS_MODE_64)
		? X86_REG_RDI
		: (_origBasicMode == CS_MODE_16 ? X86_REG_DI : X86_REG_EDI);
	Value* ptr = loadRegister(di, irb);
	auto* i8 = irb.getInt8Ty();
	for (unsigned lane = 0; lane < 16; ++lane)
	{
		Value* addr = irb.CreateAdd(ptr, ConstantInt::get(ptr->getType(), lane));
		Value* old = loadIntPtr(irb, addr, i8);
		Value* take = irb.CreateICmpSLT(irb.CreateExtractElement(mv, lane), ConstantInt::get(i8, 0));
		storeIntPtr(irb, irb.CreateSelect(take, irb.CreateExtractElement(dv, lane), old), addr, i8);
	}
}

void Capstone2LlvmIrTranslatorX86_impl::translateAvxMaskMov(cs_insn* i, cs_x86* xi, IRBuilder<>& irb)
{
	if (xi->op_count != 3)
	{
		translatePseudoAsmGeneric(i, xi, irb);
		return;
	}
	unsigned elemBits = 32;
	switch (i->id)
	{
	case X86_INS_VMASKMOVPD:
	case X86_INS_VPMASKMOVQ: elemBits = 64; break;
	case X86_INS_VMASKMOVPS:
	case X86_INS_VPMASKMOVD: elemBits = 32; break;
	default: translatePseudoAsmGeneric(i, xi, irb); return;
	}

	bool isStore = xi->operands[0].type == X86_OP_MEM;
	cs_x86_op& memOp = isStore ? xi->operands[0] : xi->operands[2];
	cs_x86_op& maskOp = xi->operands[1];
	cs_x86_op& dataOp = isStore ? xi->operands[2] : xi->operands[0];
	if (memOp.type != X86_OP_MEM)
	{
		translatePseudoAsmGeneric(i, xi, irb);
		return;
	}

	unsigned destBits = (isStore ? dataOp.size : dataOp.size) * 8;
	if (destBits != 128 && destBits != 256)
	{
		destBits = 128;
	}
	unsigned n = destBits / elemBits;
	unsigned bits = destBits;
	Value* mask = loadVectorOp(maskOp, irb, bits);
	bits = destBits;
	Value* data = loadVectorOp(dataOp, irb, bits);
	if (mask == nullptr || data == nullptr)
	{
		translatePseudoAsmGeneric(i, xi, irb);
		return;
	}
	auto* elemTy = irb.getIntNTy(elemBits);
	auto* vecTy = vecType(elemTy, n);
	auto* intTy = irb.getIntNTy(destBits);
	Value* mv = irb.CreateBitCast(irb.CreateZExtOrTrunc(mask, intTy), vecTy);
	Value* dv = irb.CreateBitCast(irb.CreateZExtOrTrunc(data, intTy), vecTy);

	Type* addrTy = getDefaultType();
	Value* base = (memOp.mem.base != X86_REG_INVALID)
		? irb.CreateSExtOrTrunc(loadRegister(memOp.mem.base, irb), addrTy)
		: ConstantInt::get(addrTy, 0);
	Value* disp = ConstantInt::get(addrTy, static_cast<uint64_t>(memOp.mem.disp), true);
	Value* addr0 = irb.CreateAdd(base, disp);
	unsigned as = getAddrSpace(memOp.mem.segment);

	if (isStore)
	{
		for (unsigned lane = 0; lane < n; ++lane)
		{
			Value* ea = irb.CreateAdd(addr0, ConstantInt::get(addrTy, lane * (elemBits / 8)));
			Value* old = loadIntPtr(irb, ea, elemTy, as);
			Value* take = irb.CreateICmpSLT(irb.CreateExtractElement(mv, lane), Constant::getNullValue(elemTy));
			storeIntPtr(irb, irb.CreateSelect(take, irb.CreateExtractElement(dv, lane), old), ea, elemTy, as);
		}
		return;
	}

	Value* res = dv;
	for (unsigned lane = 0; lane < n; ++lane)
	{
		Value* ea = irb.CreateAdd(addr0, ConstantInt::get(addrTy, lane * (elemBits / 8)));
		Value* loaded = loadIntPtr(irb, ea, elemTy, as);
		Value* take = irb.CreateICmpSLT(irb.CreateExtractElement(mv, lane), Constant::getNullValue(elemTy));
		res = irb.CreateInsertElement(res, irb.CreateSelect(take, loaded, irb.CreateExtractElement(dv, lane)), lane);
	}
	storeVectorOp(dataOp, irb.CreateBitCast(res, intTy), destBits, irb);
}

void Capstone2LlvmIrTranslatorX86_impl::translateAvxAlign(cs_insn* i, cs_x86* xi, IRBuilder<>& irb)
{
	if (xi->op_count < 4 || xi->operands[xi->op_count - 1].type != X86_OP_IMM)
	{
		translatePseudoAsmGeneric(i, xi, irb);
		return;
	}
	unsigned elemBits = (i->id == X86_INS_VALIGNQ) ? 64u : 32u;
	unsigned destBits = xi->operands[0].size * 8;
	if ((destBits != 128 && destBits != 256 && destBits != 512) || destBits % elemBits != 0)
	{
		translatePseudoAsmGeneric(i, xi, irb);
		return;
	}
	unsigned n = destBits / elemBits;
	unsigned imm = static_cast<unsigned>(xi->operands[xi->op_count - 1].imm) & (n - 1);
	unsigned bits = destBits;
	Value* src1 = loadVectorOp(xi->operands[1], irb, bits);
	bits = destBits;
	Value* src2 = loadVectorOp(xi->operands[2], irb, bits);
	if (src1 == nullptr || src2 == nullptr)
	{
		translatePseudoAsmGeneric(i, xi, irb);
		return;
	}
	// Intel Concatenate(SRC1, SRC2): SRC2 is the low destBits, SRC1 the high.
	// Right-shift by imm elements. Integer concat avoids ShuffleVector, whose
	// second-vector indexes are not reliable in the test interpreter.
	auto* intTy = irb.getIntNTy(destBits);
	auto* wideTy = irb.getIntNTy(destBits * 2);
	Value* lo = irb.CreateZExt(irb.CreateZExtOrTrunc(src2, intTy), wideTy);
	Value* hi = irb.CreateZExt(irb.CreateZExtOrTrunc(src1, intTy), wideTy);
	Value* concat = irb.CreateOr(lo, irb.CreateShl(hi, llvm::ConstantInt::get(wideTy, destBits)));
	Value* shifted = irb.CreateLShr(concat, llvm::ConstantInt::get(wideTy, static_cast<uint64_t>(imm) * elemBits));
	storeVectorOp(xi->operands[0], irb.CreateTrunc(shifted, intTy), destBits, irb);
}

void Capstone2LlvmIrTranslatorX86_impl::translateAvxBlendM(cs_insn* i, cs_x86* xi, IRBuilder<>& irb)
{
	unsigned elemBits = 0;
	switch (i->id)
	{
	case X86_INS_VPBLENDMB: elemBits = 8; break;
	case X86_INS_VPBLENDMW: elemBits = 16; break;
	case X86_INS_VBLENDMPS:
	case X86_INS_VPBLENDMD: elemBits = 32; break;
	case X86_INS_VBLENDMPD:
	case X86_INS_VPBLENDMQ: elemBits = 64; break;
	default: translatePseudoAsmGeneric(i, xi, irb); return;
	}
	if (xi->op_count < 3)
	{
		translatePseudoAsmGeneric(i, xi, irb);
		return;
	}
	unsigned destBits = xi->operands[0].size * 8;
	if (destBits != 128 && destBits != 256 && destBits != 512)
	{
		translatePseudoAsmGeneric(i, xi, irb);
		return;
	}
	unsigned n = destBits / elemBits;
	unsigned src1i = 1;
	unsigned src2i = 2;
	int kOp = -1;
	for (unsigned k = 0; k < xi->op_count; ++k)
	{
		if (xi->operands[k].type == X86_OP_REG && xi->operands[k].reg >= X86_REG_K0
			&& xi->operands[k].reg <= X86_REG_K7)
		{
			kOp = static_cast<int>(k);
		}
	}
	unsigned bits = destBits;
	Value* a = loadVectorOp(xi->operands[src1i], irb, bits);
	bits = destBits;
	Value* b = loadVectorOp(xi->operands[src2i], irb, bits);
	if (a == nullptr || b == nullptr)
	{
		translatePseudoAsmGeneric(i, xi, irb);
		return;
	}
	auto* elemTy = irb.getIntNTy(elemBits);
	auto* vecTy = vecType(elemTy, n);
	auto* intTy = irb.getIntNTy(destBits);
	Value* va = irb.CreateBitCast(irb.CreateZExtOrTrunc(a, intTy), vecTy);
	Value* vb = irb.CreateBitCast(irb.CreateZExtOrTrunc(b, intTy), vecTy);
	Value* take = nullptr;
	if (kOp >= 0)
	{
		Value* k = irb.CreateZExtOrTrunc(loadRegister(xi->operands[kOp].reg, irb), irb.getIntNTy(n > 64 ? 64 : n));
		take = UndefValue::get(FixedVectorType::get(irb.getInt1Ty(), n));
		for (unsigned lane = 0; lane < n; ++lane)
		{
			Value* bit = irb.CreateTrunc(irb.CreateLShr(k, ConstantInt::get(k->getType(), lane)), irb.getInt1Ty());
			take = irb.CreateInsertElement(take, bit, lane);
		}
	}
	else
	{
		take = Constant::getAllOnesValue(FixedVectorType::get(irb.getInt1Ty(), n));
	}
	bool zeroing = xi->operands[0].avx_zero_opmask;
	Value* keep = zeroing ? static_cast<Value*>(Constant::getNullValue(vecTy)) : va;
	Value* res = irb.CreateSelect(take, vb, keep);
	storeVectorOp(xi->operands[0], irb.CreateBitCast(res, intTy), destBits, irb);
}

void Capstone2LlvmIrTranslatorX86_impl::translateAvxExtractInsert(cs_insn* i, cs_x86* xi, IRBuilder<>& irb)
{
	bool isInsert = false;
	unsigned chunkBits = 128;
	switch (i->id)
	{
	case X86_INS_VINSERTF32X4:
	case X86_INS_VINSERTI32X4:
	case X86_INS_VINSERTF64X2:
	case X86_INS_VINSERTI64X2: isInsert = true; chunkBits = 128; break;
	case X86_INS_VINSERTF32X8:
	case X86_INS_VINSERTI32X8:
	case X86_INS_VINSERTF64X4:
	case X86_INS_VINSERTI64X4: isInsert = true; chunkBits = 256; break;
	case X86_INS_VEXTRACTF32X4:
	case X86_INS_VEXTRACTI32X4: chunkBits = 128; break;
	case X86_INS_VEXTRACTF64X4:
	case X86_INS_VEXTRACTI64X4: chunkBits = 256; break;
	default: translatePseudoAsmGeneric(i, xi, irb); return;
	}

	if (isInsert)
	{
		if (xi->op_count != 4 || xi->operands[3].type != X86_OP_IMM)
		{
			translatePseudoAsmGeneric(i, xi, irb);
			return;
		}
		unsigned destBits = xi->operands[0].size * 8;
		if (destBits != 256 && destBits != 512)
		{
			translatePseudoAsmGeneric(i, xi, irb);
			return;
		}
		unsigned nChunk = destBits / chunkBits;
		unsigned lane = static_cast<unsigned>(xi->operands[3].imm) & (nChunk - 1);
		unsigned srcBits = destBits;
		Value* src1 = loadVectorOp(xi->operands[1], irb, srcBits);
		unsigned insBits = chunkBits;
		Value* ins = loadVectorOp(xi->operands[2], irb, insBits);
		if (src1 == nullptr || ins == nullptr)
		{
			translatePseudoAsmGeneric(i, xi, irb);
			return;
		}
		auto* wide = irb.getIntNTy(destBits);
		auto* chunkTy = irb.getIntNTy(chunkBits);
		Value* w = irb.CreateZExtOrTrunc(src1, wide);
		Value* piece = irb.CreateZExt(irb.CreateZExtOrTrunc(ins, chunkTy), wide);
		Value* shift = ConstantInt::get(wide, lane * chunkBits);
		Value* mask = irb.CreateShl(irb.CreateZExt(Constant::getAllOnesValue(chunkTy), wide), shift);
		w = irb.CreateAnd(w, irb.CreateNot(mask));
		storeVectorOp(xi->operands[0], irb.CreateOr(w, irb.CreateShl(piece, shift)), destBits, irb);
		return;
	}

	if (xi->op_count != 3 || xi->operands[2].type != X86_OP_IMM)
	{
		translatePseudoAsmGeneric(i, xi, irb);
		return;
	}
	unsigned srcBits = xi->operands[1].size * 8;
	if (srcBits != 256 && srcBits != 512)
	{
		translatePseudoAsmGeneric(i, xi, irb);
		return;
	}
	unsigned nChunk = srcBits / chunkBits;
	unsigned lane = static_cast<unsigned>(xi->operands[2].imm) & (nChunk - 1);
	unsigned loadBits = srcBits;
	Value* src = loadVectorOp(xi->operands[1], irb, loadBits);
	if (src == nullptr)
	{
		translatePseudoAsmGeneric(i, xi, irb);
		return;
	}
	auto* wide = irb.getIntNTy(srcBits);
	Value* chunk = irb.CreateTrunc(
		irb.CreateLShr(irb.CreateZExtOrTrunc(src, wide), ConstantInt::get(wide, lane * chunkBits)),
		irb.getIntNTy(chunkBits));
	storeVectorOp(xi->operands[0], chunk, chunkBits, irb);
}

static llvm::Value* fpToUiDefined(llvm::Value* v, llvm::Type* intTy, llvm::IRBuilder<>& irb)
{
	unsigned bits = intTy->getScalarType()->getIntegerBitWidth();
	double bound = std::ldexp(1.0, static_cast<int>(bits));
	auto* lo = llvm::ConstantFP::get(v->getType(), 0.0);
	auto* hi = llvm::ConstantFP::get(v->getType(), bound);
	auto* inRange = irb.CreateAnd(irb.CreateFCmpOGE(v, lo), irb.CreateFCmpOLT(v, hi));
	auto* safe = irb.CreateSelect(inRange, v, lo);
	return irb.CreateSelect(inRange, irb.CreateFPToUI(safe, intTy), llvm::Constant::getNullValue(intTy));
}

void Capstone2LlvmIrTranslatorX86_impl::translateAvxCvtU(cs_insn* i, cs_x86* xi, IRBuilder<>& irb)
{
	if (xi->op_count < 2)
	{
		translatePseudoAsmGeneric(i, xi, irb);
		return;
	}
	bool toInt = false;
	bool trunc = false;
	bool fromDouble = false;
	bool scalar = false;
	bool toDouble = false;
	switch (i->id)
	{
	case X86_INS_VCVTTPD2UDQ: trunc = true; // fall through
	case X86_INS_VCVTPD2UDQ: toInt = true; fromDouble = true; break;
	case X86_INS_VCVTTPS2UDQ: trunc = true; // fall through
	case X86_INS_VCVTPS2UDQ: toInt = true; break;
	case X86_INS_VCVTTSD2USI: trunc = true; // fall through
	case X86_INS_VCVTSD2USI: toInt = true; fromDouble = true; scalar = true; break;
	case X86_INS_VCVTTSS2USI: trunc = true; // fall through
	case X86_INS_VCVTSS2USI: toInt = true; scalar = true; break;
	case X86_INS_VCVTUDQ2PD: toDouble = true; break;
	case X86_INS_VCVTUDQ2PS: break;
	case X86_INS_VCVTUSI2SD: scalar = true; toDouble = true; break;
	case X86_INS_VCVTUSI2SS: scalar = true; break;
	default: translatePseudoAsmGeneric(i, xi, irb); return;
	}

	if (scalar && toInt)
	{
		Type* fpTy = fromDouble ? static_cast<Type*>(irb.getDoubleTy()) : static_cast<Type*>(irb.getFloatTy());
		unsigned n = fromDouble ? 2u : 4u;
		Value* src = irb.CreateBitCast(toI128(loadOp(xi->operands[1], irb), irb), FixedVectorType::get(fpTy, n));
		Value* lane = irb.CreateExtractElement(src, (uint64_t)0);
		if (!trunc)
		{
			lane = irb.CreateUnaryIntrinsic(Intrinsic::roundeven, lane);
		}
		unsigned destBits = xi->operands[0].size ? xi->operands[0].size * 8 : 32;
		storeOp(xi->operands[0], fpToUiDefined(lane, irb.getIntNTy(destBits), irb), irb, eOpConv::ZEXT_TRUNC_OR_BITCAST);
		return;
	}
	if (scalar)
	{
		Value* src = loadOp(xi->operands[xi->op_count == 3 ? 2 : 1], irb);
		Type* fpTy = toDouble ? static_cast<Type*>(irb.getDoubleTy()) : static_cast<Type*>(irb.getFloatTy());
		Value* conv = irb.CreateUIToFP(src, fpTy);
		unsigned keepOp = xi->op_count == 3 ? 1u : 0u;
		unsigned n = toDouble ? 2u : 4u;
		Value* keep = irb.CreateBitCast(toI128(loadOp(xi->operands[keepOp], irb), irb), FixedVectorType::get(fpTy, n));
		storeVectorOp(
			xi->operands[0],
			irb.CreateBitCast(irb.CreateInsertElement(keep, conv, (uint64_t)0), irb.getInt128Ty()),
			128,
			irb);
		return;
	}

	unsigned srcBits = xi->operands[1].size * 8;
	if (srcBits != 128 && srcBits != 256 && srcBits != 512)
	{
		srcBits = 128;
	}
	unsigned loadBits = srcBits;
	Value* raw = loadVectorOp(xi->operands[1], irb, loadBits);
	if (raw == nullptr)
	{
		translatePseudoAsmGeneric(i, xi, irb);
		return;
	}
	if (toInt)
	{
		unsigned n = srcBits / (fromDouble ? 64u : 32u);
		auto* fpTy = fromDouble ? static_cast<Type*>(irb.getDoubleTy()) : static_cast<Type*>(irb.getFloatTy());
		Value* v = irb.CreateBitCast(irb.CreateZExtOrTrunc(raw, irb.getIntNTy(srcBits)), FixedVectorType::get(fpTy, n));
		if (!trunc)
		{
			v = irb.CreateUnaryIntrinsic(Intrinsic::roundeven, v);
		}
		auto* i32v = vecType(irb.getInt32Ty(), n);
		Value* out = UndefValue::get(i32v);
		for (unsigned lane = 0; lane < n; ++lane)
		{
			out = irb.CreateInsertElement(out, fpToUiDefined(irb.CreateExtractElement(v, lane), irb.getInt32Ty(), irb), lane);
		}
		unsigned destBits = n * 32;
		storeVectorOp(xi->operands[0], irb.CreateBitCast(out, irb.getIntNTy(destBits)), destBits >= 128 ? destBits : 128, irb);
		return;
	}

	unsigned n = srcBits / 32;
	Value* iv = irb.CreateBitCast(irb.CreateZExtOrTrunc(raw, irb.getIntNTy(srcBits)), vecType(irb.getInt32Ty(), n));
	Type* fpTy = toDouble ? static_cast<Type*>(irb.getDoubleTy()) : static_cast<Type*>(irb.getFloatTy());
	unsigned destBits = n * (toDouble ? 64u : 32u);
	auto* fVec = FixedVectorType::get(fpTy, n);
	Value* out = UndefValue::get(fVec);
	for (unsigned lane = 0; lane < n; ++lane)
	{
		out = irb.CreateInsertElement(out, irb.CreateUIToFP(irb.CreateExtractElement(iv, lane), fpTy), lane);
	}
	storeVectorOp(xi->operands[0], irb.CreateBitCast(out, irb.getIntNTy(destBits)), destBits, irb);
}

void Capstone2LlvmIrTranslatorX86_impl::translateAvxPmovNarrow(cs_insn* i, cs_x86* xi, IRBuilder<>& irb)
{
	unsigned srcElem = 0;
	unsigned dstElem = 0;
	enum Kind
	{
		Trunc,
		SSat,
		USat,
		Mask2Vec
	} kind = Trunc;
	switch (i->id)
	{
	case X86_INS_VPMOVDB: srcElem = 32; dstElem = 8; break;
	case X86_INS_VPMOVDW: srcElem = 32; dstElem = 16; break;
	case X86_INS_VPMOVQB: srcElem = 64; dstElem = 8; break;
	case X86_INS_VPMOVQD: srcElem = 64; dstElem = 32; break;
	case X86_INS_VPMOVQW: srcElem = 64; dstElem = 16; break;
	case X86_INS_VPMOVSDB: srcElem = 32; dstElem = 8; kind = SSat; break;
	case X86_INS_VPMOVSDW: srcElem = 32; dstElem = 16; kind = SSat; break;
	case X86_INS_VPMOVSQB: srcElem = 64; dstElem = 8; kind = SSat; break;
	case X86_INS_VPMOVSQD: srcElem = 64; dstElem = 32; kind = SSat; break;
	case X86_INS_VPMOVSQW: srcElem = 64; dstElem = 16; kind = SSat; break;
	case X86_INS_VPMOVUSDB: srcElem = 32; dstElem = 8; kind = USat; break;
	case X86_INS_VPMOVUSDW: srcElem = 32; dstElem = 16; kind = USat; break;
	case X86_INS_VPMOVUSQB: srcElem = 64; dstElem = 8; kind = USat; break;
	case X86_INS_VPMOVUSQD: srcElem = 64; dstElem = 32; kind = USat; break;
	case X86_INS_VPMOVUSQW: srcElem = 64; dstElem = 16; kind = USat; break;
	case X86_INS_VPMOVM2B: dstElem = 8; kind = Mask2Vec; break;
	case X86_INS_VPMOVM2W: dstElem = 16; kind = Mask2Vec; break;
	case X86_INS_VPMOVM2D: dstElem = 32; kind = Mask2Vec; break;
	case X86_INS_VPMOVM2Q: dstElem = 64; kind = Mask2Vec; break;
	case X86_INS_VPBROADCASTMW2D: dstElem = 32; kind = Mask2Vec; break;
	case X86_INS_VPBROADCASTMB2Q: dstElem = 64; kind = Mask2Vec; break;
	default: translatePseudoAsmGeneric(i, xi, irb); return;
	}

	if (kind == Mask2Vec)
	{
		if (xi->op_count != 2 || xi->operands[0].type != X86_OP_REG || xi->operands[1].type != X86_OP_REG
			|| xi->operands[1].reg < X86_REG_K0 || xi->operands[1].reg > X86_REG_K7
			|| xi->avx_sae || xi->avx_rm != X86_AVX_RM_INVALID || xi->operands[0].avx_zero_opmask)
		{
			translatePseudoAsmGeneric(i, xi, irb);
			return;
		}
		unsigned destBits = xi->operands[0].size * 8;
		if ((destBits != 128 && destBits != 256 && destBits != 512) || destBits % dstElem != 0)
		{
			translatePseudoAsmGeneric(i, xi, irb);
			return;
		}
		unsigned n = destBits / dstElem;
		Value* k = loadRegister(xi->operands[1].reg, irb);
		if (k == nullptr)
		{
			translatePseudoAsmGeneric(i, xi, irb);
			return;
		}
		k = irb.CreateZExtOrTrunc(k, irb.getInt64Ty());
		auto* elemTy = irb.getIntNTy(dstElem);
		auto* vecTy = vecType(elemTy, n);
		Value* ones = llvm::Constant::getAllOnesValue(elemTy);
		Value* zero = llvm::Constant::getNullValue(elemTy);
		Value* out = llvm::Constant::getNullValue(vecTy);
		for (unsigned lane = 0; lane < n; ++lane)
		{
			Value* bit = irb.CreateTrunc(irb.CreateLShr(k, llvm::ConstantInt::get(k->getType(), lane)), irb.getInt1Ty());
			out = irb.CreateInsertElement(out, irb.CreateSelect(bit, ones, zero), lane);
		}
		storeVectorOp(xi->operands[0], irb.CreateBitCast(out, irb.getIntNTy(destBits)), destBits, irb);
		return;
	}

	if (xi->op_count < 2 || hasEvexModifier(xi))
	{
		translatePseudoAsmGeneric(i, xi, irb);
		return;
	}

	unsigned srcBits = xi->operands[1].size * 8;
	Value* src = loadVectorOp(xi->operands[1], irb, srcBits);
	if (src == nullptr || srcBits < srcElem || srcBits % srcElem != 0)
	{
		translatePseudoAsmGeneric(i, xi, irb);
		return;
	}

	unsigned n = srcBits / srcElem;
	unsigned packedBits = n * dstElem;
	auto* srcTy = irb.getIntNTy(srcElem);
	auto* dstTy = irb.getIntNTy(dstElem);
	auto* srcVec = vecType(srcTy, n);
	auto* dstVec = vecType(dstTy, n);
	Value* vs = irb.CreateBitCast(irb.CreateZExtOrTrunc(src, irb.getIntNTy(srcBits)), srcVec);
	Value* out = llvm::UndefValue::get(dstVec);
	for (unsigned lane = 0; lane < n; ++lane)
	{
		Value* e = irb.CreateExtractElement(vs, lane);
		if (kind == SSat)
		{
			e = irb.CreateTrunc(clampToRange(e, dstElem, true, irb), dstTy);
		}
		else if (kind == USat)
		{
			e = irb.CreateTrunc(clampToRange(e, dstElem, false, irb), dstTy);
		}
		else
		{
			e = irb.CreateTrunc(e, dstTy);
		}
		out = irb.CreateInsertElement(out, e, lane);
	}

	Value* packed = irb.CreateBitCast(out, irb.getIntNTy(packedBits));
	if (xi->operands[0].type == X86_OP_REG)
	{
		unsigned destBits = xi->operands[0].size * 8;
		unsigned rb = vectorRegisterWidth(xi->operands[0].reg);
		if (rb != 0)
		{
			destBits = rb;
		}
		if (destBits != 128 && destBits != 256 && destBits != 512)
		{
			translatePseudoAsmGeneric(i, xi, irb);
			return;
		}
		storeVectorOp(xi->operands[0], irb.CreateZExtOrTrunc(packed, irb.getIntNTy(destBits)), destBits, irb);
	}
	else
	{
		unsigned destBits = xi->operands[0].size ? xi->operands[0].size * 8 : packedBits;
		storeOp(xi->operands[0], irb.CreateZExtOrTrunc(packed, irb.getIntNTy(destBits)), irb, eOpConv::NOTHING);
	}
}

void Capstone2LlvmIrTranslatorX86_impl::translateAvxPermI2(cs_insn* i, cs_x86* xi, IRBuilder<>& irb)
{
	unsigned elemBits = 0;
	bool indexIsDest = false;
	switch (i->id)
	{
	case X86_INS_VPERMI2D:
	case X86_INS_VPERMI2PS: indexIsDest = true; elemBits = 32; break;
	case X86_INS_VPERMI2Q:
	case X86_INS_VPERMI2PD: indexIsDest = true; elemBits = 64; break;
	case X86_INS_VPERMT2D:
	case X86_INS_VPERMT2PS: elemBits = 32; break;
	case X86_INS_VPERMT2Q:
	case X86_INS_VPERMT2PD: elemBits = 64; break;
	default: translatePseudoAsmGeneric(i, xi, irb); return;
	}

	if (xi->op_count < 3 || hasEvexModifier(xi))
	{
		translatePseudoAsmGeneric(i, xi, irb);
		return;
	}

	unsigned destBits = xi->operands[0].size * 8;
	if ((destBits != 128 && destBits != 256 && destBits != 512) || destBits % elemBits != 0)
	{
		translatePseudoAsmGeneric(i, xi, irb);
		return;
	}

	unsigned n = destBits / elemBits;
	unsigned b0 = destBits;
	unsigned b1 = destBits;
	unsigned b2 = destBits;
	Value* dest = loadVectorOp(xi->operands[0], irb, b0);
	Value* a = loadVectorOp(xi->operands[1], irb, b1);
	Value* b = loadVectorOp(xi->operands[2], irb, b2);
	if (dest == nullptr || a == nullptr || b == nullptr)
	{
		translatePseudoAsmGeneric(i, xi, irb);
		return;
	}

	auto* intTy = irb.getIntNTy(destBits);
	auto* elemTy = irb.getIntNTy(elemBits);
	auto* vecTy = vecType(elemTy, n);
	Value* vd = irb.CreateBitCast(irb.CreateZExtOrTrunc(dest, intTy), vecTy);
	Value* va = irb.CreateBitCast(irb.CreateZExtOrTrunc(a, intTy), vecTy);
	Value* vb = irb.CreateBitCast(irb.CreateZExtOrTrunc(b, intTy), vecTy);
	Value* index = indexIsDest ? vd : va;
	Value* table0 = indexIsDest ? va : vd;
	Value* table1 = vb;
	Value* nC = llvm::ConstantInt::get(elemTy, n);
	Value* idxMask = llvm::ConstantInt::get(elemTy, 2u * n - 1u);
	Value* out = llvm::UndefValue::get(vecTy);
	for (unsigned lane = 0; lane < n; ++lane)
	{
		Value* idx = irb.CreateAnd(irb.CreateExtractElement(index, lane), idxMask);
		Value* from0 = permuteSelectLane(table0, idx, n, irb);
		Value* from1 = permuteSelectLane(table1, irb.CreateSub(idx, nC), n, irb);
		out = irb.CreateInsertElement(out, irb.CreateSelect(irb.CreateICmpUGE(idx, nC), from1, from0), lane);
	}
	storeVectorOp(xi->operands[0], irb.CreateBitCast(out, intTy), destBits, irb);
}

void Capstone2LlvmIrTranslatorX86_impl::translateAvxCompressExpand(cs_insn* i, cs_x86* xi, IRBuilder<>& irb)
{
	bool compress = false;
	unsigned elemBits = 32;
	switch (i->id)
	{
	case X86_INS_VCOMPRESSPD:
	case X86_INS_VPCOMPRESSQ: compress = true; elemBits = 64; break;
	case X86_INS_VCOMPRESSPS:
	case X86_INS_VPCOMPRESSD: compress = true; elemBits = 32; break;
	case X86_INS_VEXPANDPD:
	case X86_INS_VPEXPANDQ: elemBits = 64; break;
	case X86_INS_VEXPANDPS:
	case X86_INS_VPEXPANDD: elemBits = 32; break;
	default: translatePseudoAsmGeneric(i, xi, irb); return;
	}
	if (xi->op_count < 2)
	{
		translatePseudoAsmGeneric(i, xi, irb);
		return;
	}
	unsigned destBits = xi->operands[0].size * 8;
	if (destBits != 128 && destBits != 256 && destBits != 512)
	{
		translatePseudoAsmGeneric(i, xi, irb);
		return;
	}
	unsigned n = destBits / elemBits;
	int kOp = -1;
	for (unsigned k = 0; k < xi->op_count; ++k)
	{
		if (xi->operands[k].type == X86_OP_REG && xi->operands[k].reg >= X86_REG_K0
			&& xi->operands[k].reg <= X86_REG_K7)
		{
			kOp = static_cast<int>(k);
		}
	}
	unsigned srcI = (xi->operands[0].type == X86_OP_MEM) ? 1u : 1u;
	unsigned bits = destBits;
	Value* src = loadVectorOp(xi->operands[srcI], irb, bits);
	if (src == nullptr)
	{
		translatePseudoAsmGeneric(i, xi, irb);
		return;
	}
	auto* elemTy = irb.getIntNTy(elemBits);
	auto* vecTy = vecType(elemTy, n);
	auto* intTy = irb.getIntNTy(destBits);
	Value* vs = irb.CreateBitCast(irb.CreateZExtOrTrunc(src, intTy), vecTy);
	Value* k = (kOp >= 0)
		? irb.CreateZExtOrTrunc(loadRegister(xi->operands[kOp].reg, irb), irb.getInt64Ty())
		: ConstantInt::get(irb.getInt64Ty(), ~0ULL);
	Value* res = Constant::getNullValue(vecTy);
	if (compress)
	{
		Value* outIdx = ConstantInt::get(irb.getInt32Ty(), 0);
		for (unsigned lane = 0; lane < n; ++lane)
		{
			Value* bit = irb.CreateTrunc(irb.CreateLShr(k, ConstantInt::get(k->getType(), lane)), irb.getInt1Ty());
			Value* elem = irb.CreateExtractElement(vs, lane);
			res = irb.CreateInsertElement(res, irb.CreateSelect(bit, elem, irb.CreateExtractElement(res, outIdx)), outIdx);
			outIdx = irb.CreateAdd(outIdx, irb.CreateZExt(bit, irb.getInt32Ty()));
		}
	}
	else
	{
		Value* inIdx = ConstantInt::get(irb.getInt32Ty(), 0);
		for (unsigned lane = 0; lane < n; ++lane)
		{
			Value* bit = irb.CreateTrunc(irb.CreateLShr(k, ConstantInt::get(k->getType(), lane)), irb.getInt1Ty());
			Value* elem = irb.CreateExtractElement(vs, inIdx);
			res = irb.CreateInsertElement(res, irb.CreateSelect(bit, elem, Constant::getNullValue(elemTy)), lane);
			inIdx = irb.CreateAdd(inIdx, irb.CreateZExt(bit, irb.getInt32Ty()));
		}
	}
	if (xi->operands[0].type == X86_OP_MEM)
	{
		storeOp(xi->operands[0], irb.CreateBitCast(res, intTy), irb, eOpConv::NOTHING);
	}
	else
	{
		storeVectorOp(xi->operands[0], irb.CreateBitCast(res, intTy), destBits, irb);
	}
}

void Capstone2LlvmIrTranslatorX86_impl::translateAvxLzcnt(cs_insn* i, cs_x86* xi, IRBuilder<>& irb)
{
	unsigned elemBits = (i->id == X86_INS_VPLZCNTQ) ? 64u : 32u;
	if (xi->op_count < 2)
	{
		translatePseudoAsmGeneric(i, xi, irb);
		return;
	}
	unsigned destBits = xi->operands[0].size * 8;
	if ((destBits != 128 && destBits != 256 && destBits != 512) || destBits % elemBits != 0)
	{
		translatePseudoAsmGeneric(i, xi, irb);
		return;
	}
	unsigned bits = destBits;
	Value* src = loadVectorOp(xi->operands[1], irb, bits);
	if (src == nullptr)
	{
		translatePseudoAsmGeneric(i, xi, irb);
		return;
	}
	unsigned n = destBits / elemBits;
	auto* intTy = irb.getIntNTy(destBits);
	auto* elemInt = irb.getIntNTy(elemBits);
	auto* ctlz = llvm::Intrinsic::getOrInsertDeclaration(_module, llvm::Intrinsic::ctlz, {elemInt});
	src = irb.CreateZExtOrTrunc(src, intTy);
	Value* out = llvm::ConstantInt::get(intTy, 0);
	Value* laneMask0 = irb.CreateZExt(llvm::Constant::getAllOnesValue(elemInt), intTy);
	for (unsigned lane = 0; lane < n; ++lane)
	{
		Value* shift = llvm::ConstantInt::get(intTy, static_cast<uint64_t>(lane) * elemBits);
		Value* e = irb.CreateTrunc(irb.CreateLShr(src, shift), elemInt);
		Value* cnt = irb.CreateCall(ctlz, {e, irb.getFalse()});
		Value* laneMask = irb.CreateShl(laneMask0, shift);
		out = irb.CreateOr(irb.CreateAnd(out, irb.CreateNot(laneMask)), irb.CreateShl(irb.CreateZExt(cnt, intTy), shift));
	}
	storeVectorOp(xi->operands[0], out, destBits, irb);
}

void Capstone2LlvmIrTranslatorX86_impl::translateAvxScatter(cs_insn* i, cs_x86* xi, IRBuilder<>& irb)
{
	if (xi->op_count < 2)
	{
		translatePseudoAsmGeneric(i, xi, irb);
		return;
	}
	int memI = -1;
	int srcI = -1;
	int kOp = -1;
	for (unsigned k = 0; k < xi->op_count; ++k)
	{
		if (xi->operands[k].type == X86_OP_MEM)
		{
			memI = static_cast<int>(k);
		}
		else if (xi->operands[k].type == X86_OP_REG && xi->operands[k].reg >= X86_REG_K0
			&& xi->operands[k].reg <= X86_REG_K7)
		{
			kOp = static_cast<int>(k);
		}
		else if (xi->operands[k].type == X86_OP_REG)
		{
			srcI = static_cast<int>(k);
		}
	}
	if (memI < 0 || srcI < 0 || xi->operands[memI].mem.index == X86_REG_INVALID)
	{
		translatePseudoAsmGeneric(i, xi, irb);
		return;
	}

	unsigned dataBits = 32;
	unsigned idxBits = 32;
	switch (i->id)
	{
	case X86_INS_VPSCATTERDD:
	case X86_INS_VSCATTERDPS: dataBits = 32; idxBits = 32; break;
	case X86_INS_VPSCATTERDQ:
	case X86_INS_VSCATTERDPD: dataBits = 64; idxBits = 32; break;
	case X86_INS_VPSCATTERQD:
	case X86_INS_VSCATTERQPS: dataBits = 32; idxBits = 64; break;
	case X86_INS_VPSCATTERQQ:
	case X86_INS_VSCATTERQPD: dataBits = 64; idxBits = 64; break;
	default: translatePseudoAsmGeneric(i, xi, irb); return;
	}

	unsigned destBits = xi->operands[srcI].size * 8;
	if (destBits != 128 && destBits != 256 && destBits != 512)
	{
		translatePseudoAsmGeneric(i, xi, irb);
		return;
	}
	unsigned n = (idxBits == 64 && dataBits == 32) ? (destBits / 64) : (destBits / dataBits);
	unsigned bits = destBits;
	Value* src = loadVectorOp(xi->operands[srcI], irb, bits);
	if (src == nullptr)
	{
		translatePseudoAsmGeneric(i, xi, irb);
		return;
	}
	auto& mem = xi->operands[memI].mem;
	cs_x86_op idxOp{};
	idxOp.type = X86_OP_REG;
	idxOp.reg = mem.index;
	idxOp.size = static_cast<uint8_t>(vectorRegisterWidth(mem.index) / 8);
	unsigned idxRegBits = idxOp.size * 8;
	Value* idxRaw = loadVectorOp(idxOp, irb, idxRegBits);
	if (idxRaw == nullptr)
	{
		translatePseudoAsmGeneric(i, xi, irb);
		return;
	}
	auto* dataTy = irb.getIntNTy(dataBits);
	auto* destVec = vecType(dataTy, destBits / dataBits);
	auto* idxVec = vecType(irb.getIntNTy(idxBits), idxRegBits / idxBits);
	Value* sv = irb.CreateBitCast(irb.CreateZExtOrTrunc(src, irb.getIntNTy(destBits)), destVec);
	Value* iv = irb.CreateBitCast(irb.CreateZExtOrTrunc(idxRaw, irb.getIntNTy(idxRegBits)), idxVec);
	Value* k = (kOp >= 0)
		? irb.CreateZExtOrTrunc(loadRegister(xi->operands[kOp].reg, irb), irb.getInt64Ty())
		: ConstantInt::get(irb.getInt64Ty(), ~0ULL);
	Type* addrTy = getDefaultType();
	Value* base = (mem.base != X86_REG_INVALID)
		? irb.CreateSExtOrTrunc(loadRegister(mem.base, irb), addrTy)
		: ConstantInt::get(addrTy, 0);
	Value* disp = ConstantInt::get(addrTy, static_cast<uint64_t>(mem.disp), true);
	Value* scale = ConstantInt::get(addrTy, mem.scale ? mem.scale : 1);
	unsigned as = getAddrSpace(mem.segment);
	for (unsigned lane = 0; lane < n; ++lane)
	{
		Value* idx = irb.CreateSExtOrTrunc(irb.CreateExtractElement(iv, lane), addrTy);
		Value* ea = irb.CreateAdd(irb.CreateAdd(base, disp), irb.CreateMul(idx, scale));
		Value* old = loadIntPtr(irb, ea, dataTy, as);
		Value* bit = irb.CreateTrunc(irb.CreateLShr(k, ConstantInt::get(k->getType(), lane)), irb.getInt1Ty());
		storeIntPtr(irb, irb.CreateSelect(bit, irb.CreateExtractElement(sv, lane), old), ea, dataTy, as);
	}
}

static llvm::Value* rotateLane(llvm::Value* v, llvm::Value* amt, unsigned elemBits, llvm::IRBuilder<>& irb)
{
	auto* ty = v->getType();
	llvm::Value* mask = llvm::ConstantInt::get(ty, elemBits - 1);
	llvm::Value* a = irb.CreateAnd(amt, mask);
	llvm::Value* left = irb.CreateShl(v, a);
	llvm::Value* right = irb.CreateLShr(v, irb.CreateAnd(irb.CreateSub(llvm::ConstantInt::get(ty, elemBits), a), mask));
	return irb.CreateOr(left, right);
}

void Capstone2LlvmIrTranslatorX86_impl::translateAvxConflict(cs_insn* i, cs_x86* xi, IRBuilder<>& irb)
{
	if (xi->op_count != 2)
	{
		translatePseudoAsmGeneric(i, xi, irb);
		return;
	}
	unsigned elemBits = (i->id == X86_INS_VPCONFLICTQ) ? 64u : 32u;
	unsigned destBits = xi->operands[0].size * 8;
	if ((destBits != 128 && destBits != 256 && destBits != 512) || destBits % elemBits != 0)
	{
		translatePseudoAsmGeneric(i, xi, irb);
		return;
	}
	unsigned bits = destBits;
	Value* src = loadVectorOp(xi->operands[1], irb, bits);
	if (src == nullptr)
	{
		translatePseudoAsmGeneric(i, xi, irb);
		return;
	}
	unsigned n = destBits / elemBits;
	auto* elemTy = irb.getIntNTy(elemBits);
	auto* vecTy = vecType(elemTy, n);
	Value* v = irb.CreateBitCast(irb.CreateZExtOrTrunc(src, irb.getIntNTy(destBits)), vecTy);
	Value* out = Constant::getNullValue(vecTy);
	for (unsigned lane = 0; lane < n; ++lane)
	{
		Value* acc = Constant::getNullValue(elemTy);
		Value* cur = irb.CreateExtractElement(v, lane);
		for (unsigned j = 0; j < lane; ++j)
		{
			Value* eq = irb.CreateICmpEQ(cur, irb.CreateExtractElement(v, j));
			Value* bit = ConstantInt::get(elemTy, uint64_t(1) << j);
			acc = irb.CreateOr(acc, irb.CreateSelect(eq, bit, Constant::getNullValue(elemTy)));
		}
		out = irb.CreateInsertElement(out, acc, lane);
	}
	storeVectorOp(xi->operands[0], irb.CreateBitCast(out, irb.getIntNTy(destBits)), destBits, irb);
}

void Capstone2LlvmIrTranslatorX86_impl::translate3dnow(cs_insn* i, cs_x86* xi, IRBuilder<>& irb)
{
	if (i->id == X86_INS_FEMMS)
	{
		return;
	}
	if (xi->op_count != 2)
	{
		translatePseudoAsmGeneric(i, xi, irb);
		return;
	}

	auto* i64 = irb.getInt64Ty();
	Value* dst = irb.CreateZExtOrTrunc(loadOp(xi->operands[0], irb), i64);
	Value* src = irb.CreateZExtOrTrunc(loadOp(xi->operands[1], irb), i64);
	auto* v2f = vecType(irb.getFloatTy(), 2);
	auto* v2i = vecType(irb.getInt32Ty(), 2);
	auto storeI64 = [&](Value* v) {
		storeOp(xi->operands[0], irb.CreateBitCast(v, i64), irb, eOpConv::ZEXT_TRUNC_OR_BITCAST);
	};

	switch (i->id)
	{
	case X86_INS_PFADD:
	case X86_INS_PFSUB:
	case X86_INS_PFSUBR:
	case X86_INS_PFMUL:
	case X86_INS_PFMAX:
	case X86_INS_PFMIN:
	{
		Value* a = irb.CreateBitCast(dst, v2f);
		Value* b = irb.CreateBitCast(src, v2f);
		Value* r = nullptr;
		if (i->id == X86_INS_PFADD) r = irb.CreateFAdd(a, b);
		else if (i->id == X86_INS_PFSUB) r = irb.CreateFSub(a, b);
		else if (i->id == X86_INS_PFSUBR) r = irb.CreateFSub(b, a);
		else if (i->id == X86_INS_PFMUL) r = irb.CreateFMul(a, b);
		else if (i->id == X86_INS_PFMAX) r = irb.CreateSelect(irb.CreateFCmpOGT(a, b), a, b);
		else r = irb.CreateSelect(irb.CreateFCmpOLT(a, b), a, b);
		storeI64(r);
		return;
	}
	case X86_INS_PFCMPEQ:
	case X86_INS_PFCMPGE:
	case X86_INS_PFCMPGT:
	{
		Value* a = irb.CreateBitCast(dst, v2f);
		Value* b = irb.CreateBitCast(src, v2f);
		Value* c = nullptr;
		if (i->id == X86_INS_PFCMPEQ) c = irb.CreateFCmpOEQ(a, b);
		else if (i->id == X86_INS_PFCMPGE) c = irb.CreateFCmpOGE(a, b);
		else c = irb.CreateFCmpOGT(a, b);
		storeI64(irb.CreateSExt(c, v2i));
		return;
	}
	case X86_INS_PFRCP:
	{
		Value* one = irb.CreateVectorSplat(2, ConstantFP::get(irb.getFloatTy(), 1.0));
		storeI64(irb.CreateFDiv(one, irb.CreateBitCast(src, v2f)));
		return;
	}
	case X86_INS_PFRSQRT:
	{
		Value* one = irb.CreateVectorSplat(2, ConstantFP::get(irb.getFloatTy(), 1.0));
		Value* v = irb.CreateBitCast(src, v2f);
		storeI64(irb.CreateFDiv(one, irb.CreateUnaryIntrinsic(Intrinsic::sqrt, v)));
		return;
	}
	case X86_INS_PI2FD:
		storeI64(irb.CreateSIToFP(irb.CreateBitCast(src, v2i), v2f));
		return;
	case X86_INS_PF2ID:
		storeI64(irb.CreateFPToSI(irb.CreateBitCast(src, v2f), v2i));
		return;
	case X86_INS_PSWAPD:
	{
		Value* v = irb.CreateBitCast(src, v2i);
		storeI64(irb.CreateShuffleVector(v, v, std::vector<int>{1, 0}));
		return;
	}
	case X86_INS_PAVGUSB:
	{
		auto* v8 = vecType(irb.getInt8Ty(), 8);
		auto* wide = vecType(irb.getInt16Ty(), 8);
		Value* a = irb.CreateZExt(irb.CreateBitCast(dst, v8), wide);
		Value* b = irb.CreateZExt(irb.CreateBitCast(src, v8), wide);
		Value* one = ConstantInt::get(wide, 1);
		storeI64(irb.CreateBitCast(
			irb.CreateTrunc(irb.CreateLShr(irb.CreateAdd(irb.CreateAdd(a, b), one), one), v8), i64));
		return;
	}
	case X86_INS_PMULHRW:
	{
		auto* v4 = vecType(irb.getInt16Ty(), 4);
		auto* wide = vecType(irb.getInt32Ty(), 4);
		Value* a = irb.CreateSExt(irb.CreateBitCast(dst, v4), wide);
		Value* b = irb.CreateSExt(irb.CreateBitCast(src, v4), wide);
		Value* prod = irb.CreateAdd(irb.CreateMul(a, b), ConstantInt::get(wide, 0x8000));
		storeI64(irb.CreateBitCast(irb.CreateTrunc(irb.CreateLShr(prod, ConstantInt::get(wide, 16)), v4), i64));
		return;
	}
	default:
		translatePseudoAsmGeneric(i, xi, irb);
		return;
	}
}

void Capstone2LlvmIrTranslatorX86_impl::translateXop(cs_insn* i, cs_x86* xi, IRBuilder<>& irb)
{
	if (i->id == X86_INS_VPCOM || i->id == X86_INS_VPCOMB || i->id == X86_INS_VPCOMW
		|| i->id == X86_INS_VPCOMD || i->id == X86_INS_VPCOMQ || i->id == X86_INS_VPCOMUB
		|| i->id == X86_INS_VPCOMUW || i->id == X86_INS_VPCOMUD || i->id == X86_INS_VPCOMUQ)
	{
		unsigned elemBits = 0;
		bool isUnsigned = false;
		switch (i->id)
		{
		case X86_INS_VPCOMB: elemBits = 8; break;
		case X86_INS_VPCOMW: elemBits = 16; break;
		case X86_INS_VPCOMD: elemBits = 32; break;
		case X86_INS_VPCOMQ: elemBits = 64; break;
		case X86_INS_VPCOMUB: elemBits = 8; isUnsigned = true; break;
		case X86_INS_VPCOMUW: elemBits = 16; isUnsigned = true; break;
		case X86_INS_VPCOMUD: elemBits = 32; isUnsigned = true; break;
		case X86_INS_VPCOMUQ: elemBits = 64; isUnsigned = true; break;
		default:
		{
			uint8_t opc = (i->size >= 4 && i->bytes[0] == 0x8f) ? i->bytes[3] : 0;
			switch (opc)
			{
			case 0xCC: elemBits = 8; break;
			case 0xCD: elemBits = 16; break;
			case 0xCE: elemBits = 32; break;
			case 0xCF: elemBits = 64; break;
			case 0xEC: elemBits = 8; isUnsigned = true; break;
			case 0xED: elemBits = 16; isUnsigned = true; break;
			case 0xEE: elemBits = 32; isUnsigned = true; break;
			case 0xEF: elemBits = 64; isUnsigned = true; break;
			default: translatePseudoAsmGeneric(i, xi, irb); return;
			}
			break;
		}
		}
		uint8_t pred = 0;
		if (xi->op_count >= 4 && xi->operands[xi->op_count - 1].type == X86_OP_IMM)
		{
			pred = static_cast<uint8_t>(xi->operands[xi->op_count - 1].imm);
		}
		else if (xi->encoding.imm_size >= 1 && xi->encoding.imm_offset < i->size)
		{
			pred = i->bytes[xi->encoding.imm_offset];
		}
		else
		{
			translatePseudoAsmGeneric(i, xi, irb);
			return;
		}
		if (xi->op_count < 3)
		{
			translatePseudoAsmGeneric(i, xi, irb);
			return;
		}
		unsigned destBits = xi->operands[0].size * 8;
		if (destBits != 128 && destBits != 256)
		{
			destBits = 128;
		}
		if (elemBits == 0 || destBits % elemBits != 0)
		{
			translatePseudoAsmGeneric(i, xi, irb);
			return;
		}
		unsigned bits = destBits;
		Value* aRaw = loadVectorOp(xi->operands[1], irb, bits);
		bits = destBits;
		Value* bRaw = loadVectorOp(xi->operands[2], irb, bits);
		if (aRaw == nullptr || bRaw == nullptr)
		{
			translatePseudoAsmGeneric(i, xi, irb);
			return;
		}
		unsigned n = destBits / elemBits;
		auto* elemTy = irb.getIntNTy(elemBits);
		auto* vecTy = vecType(elemTy, n);
		auto* intTy = irb.getIntNTy(destBits);
		Value* va = irb.CreateBitCast(irb.CreateZExtOrTrunc(aRaw, intTy), vecTy);
		Value* vb = irb.CreateBitCast(irb.CreateZExtOrTrunc(bRaw, intTy), vecTy);
		Value* cmp = nullptr;
		pred &= 7;
		if (pred == 6)
		{
			storeVectorOp(xi->operands[0], Constant::getNullValue(intTy), destBits, irb);
			return;
		}
		if (pred == 7)
		{
			storeVectorOp(xi->operands[0], Constant::getAllOnesValue(intTy), destBits, irb);
			return;
		}
		switch (pred)
		{
		case 0: cmp = isUnsigned ? irb.CreateICmpULT(va, vb) : irb.CreateICmpSLT(va, vb); break;
		case 1: cmp = isUnsigned ? irb.CreateICmpULE(va, vb) : irb.CreateICmpSLE(va, vb); break;
		case 2: cmp = isUnsigned ? irb.CreateICmpUGT(va, vb) : irb.CreateICmpSGT(va, vb); break;
		case 3: cmp = isUnsigned ? irb.CreateICmpUGE(va, vb) : irb.CreateICmpSGE(va, vb); break;
		case 4: cmp = irb.CreateICmpEQ(va, vb); break;
		default: cmp = irb.CreateICmpNE(va, vb); break;
		}
		storeVectorOp(xi->operands[0], irb.CreateBitCast(irb.CreateSExt(cmp, vecTy), intTy), destBits, irb);
		return;
	}

	if (i->id == X86_INS_VFRCZPS || i->id == X86_INS_VFRCZPD || i->id == X86_INS_VFRCZSS || i->id == X86_INS_VFRCZSD)
	{
		if (xi->op_count != 2)
		{
			translatePseudoAsmGeneric(i, xi, irb);
			return;
		}
		bool isDouble = i->id == X86_INS_VFRCZPD || i->id == X86_INS_VFRCZSD;
		bool isScalar = i->id == X86_INS_VFRCZSS || i->id == X86_INS_VFRCZSD;
		unsigned destBits = xi->operands[0].size * 8;
		if (destBits != 128 && destBits != 256)
		{
			destBits = 128;
		}
		unsigned bits = destBits;
		Value* src = loadVectorOp(xi->operands[1], irb, bits);
		if (src == nullptr)
		{
			src = toI128(loadOp(xi->operands[1], irb), irb);
			destBits = 128;
		}
		Type* elemTy = isDouble ? static_cast<Type*>(irb.getDoubleTy()) : static_cast<Type*>(irb.getFloatTy());
		unsigned n = destBits / (isDouble ? 64u : 32u);
		auto* vecTy = FixedVectorType::get(elemTy, n);
		Value* v = irb.CreateBitCast(irb.CreateZExtOrTrunc(src, irb.getIntNTy(destBits)), vecTy);
		Value* frac = irb.CreateFSub(v, irb.CreateUnaryIntrinsic(Intrinsic::trunc, v));
		if (isScalar)
		{
			Value* keep = irb.CreateBitCast(irb.CreateZExtOrTrunc(src, irb.getIntNTy(destBits)), vecTy);
			frac = irb.CreateInsertElement(keep, irb.CreateExtractElement(frac, (uint64_t)0), (uint64_t)0);
		}
		storeVectorOp(xi->operands[0], irb.CreateBitCast(frac, irb.getIntNTy(destBits)), destBits, irb);
		return;
	}

	if (i->id == X86_INS_VPCMOV)
	{
		if (xi->op_count != 4)
		{
			translatePseudoAsmGeneric(i, xi, irb);
			return;
		}
		unsigned destBits = xi->operands[0].size * 8;
		if (destBits != 128 && destBits != 256)
		{
			translatePseudoAsmGeneric(i, xi, irb);
			return;
		}
		unsigned bits = destBits;
		Value* a = loadVectorOp(xi->operands[1], irb, bits);
		bits = destBits;
		Value* b = loadVectorOp(xi->operands[2], irb, bits);
		bits = destBits;
		Value* c = loadVectorOp(xi->operands[3], irb, bits);
		if (a == nullptr || b == nullptr || c == nullptr)
		{
			translatePseudoAsmGeneric(i, xi, irb);
			return;
		}
		auto* intTy = irb.getIntNTy(destBits);
		a = irb.CreateZExtOrTrunc(a, intTy);
		b = irb.CreateZExtOrTrunc(b, intTy);
		c = irb.CreateZExtOrTrunc(c, intTy);
		storeVectorOp(xi->operands[0], irb.CreateOr(irb.CreateAnd(a, c), irb.CreateAnd(b, irb.CreateNot(c))), destBits, irb);
		return;
	}

	unsigned elemBits = 0;
	bool rot = false;
	bool arith = false;
	switch (i->id)
	{
	case X86_INS_VPROTB: rot = true; elemBits = 8; break;
	case X86_INS_VPROTW: rot = true; elemBits = 16; break;
	case X86_INS_VPROTD: rot = true; elemBits = 32; break;
	case X86_INS_VPROTQ: rot = true; elemBits = 64; break;
	case X86_INS_VPSHAB: arith = true; elemBits = 8; break;
	case X86_INS_VPSHAW: arith = true; elemBits = 16; break;
	case X86_INS_VPSHAD: arith = true; elemBits = 32; break;
	case X86_INS_VPSHAQ: arith = true; elemBits = 64; break;
	case X86_INS_VPSHLB: elemBits = 8; break;
	case X86_INS_VPSHLW: elemBits = 16; break;
	case X86_INS_VPSHLD: elemBits = 32; break;
	case X86_INS_VPSHLQ: elemBits = 64; break;
	case X86_INS_VPPERM: break;
	default: translatePseudoAsmGeneric(i, xi, irb); return;
	}

	if (i->id == X86_INS_VPPERM)
	{
		if (xi->op_count != 4)
		{
			translatePseudoAsmGeneric(i, xi, irb);
			return;
		}
		unsigned bits = 128;
		Value* a = loadVectorOp(xi->operands[1], irb, bits);
		bits = 128;
		Value* b = loadVectorOp(xi->operands[2], irb, bits);
		bits = 128;
		Value* sel = loadVectorOp(xi->operands[3], irb, bits);
		if (a == nullptr || b == nullptr || sel == nullptr)
		{
			translatePseudoAsmGeneric(i, xi, irb);
			return;
		}
		auto* i8v = vecType(irb.getInt8Ty(), 16);
		Value* va = irb.CreateBitCast(toI128(a, irb), i8v);
		Value* vb = irb.CreateBitCast(toI128(b, irb), i8v);
		Value* vs = irb.CreateBitCast(toI128(sel, irb), i8v);
		Value* cat = concatLane128(toI128(b, irb), toI128(a, irb), irb);
		auto* i8x32 = vecType(irb.getInt8Ty(), 32);
		Value* pool = irb.CreateBitCast(cat, i8x32);
		Value* res = UndefValue::get(i8v);
		for (unsigned lane = 0; lane < 16; ++lane)
		{
			Value* s = irb.CreateExtractElement(vs, lane);
			Value* idx = irb.CreateAnd(s, ConstantInt::get(irb.getInt8Ty(), 0x1f));
			Value* elem = irb.CreateExtractElement(pool, irb.CreateZExt(idx, irb.getInt32Ty()));
			Value* flip = irb.CreateICmpSLT(s, ConstantInt::get(irb.getInt8Ty(), 0));
			elem = irb.CreateSelect(flip, irb.CreateNot(elem), elem);
			res = irb.CreateInsertElement(res, elem, lane);
		}
		(void)va;
		(void)vb;
		storeVectorOp(xi->operands[0], irb.CreateBitCast(res, irb.getInt128Ty()), 128, irb);
		return;
	}

	if (xi->op_count < 3)
	{
		translatePseudoAsmGeneric(i, xi, irb);
		return;
	}
	unsigned destBits = xi->operands[0].size * 8;
	if (destBits != 128 && destBits != 256)
	{
		translatePseudoAsmGeneric(i, xi, irb);
		return;
	}
	unsigned n = destBits / elemBits;
	unsigned bits = destBits;
	Value* src = loadVectorOp(xi->operands[1], irb, bits);
	if (src == nullptr)
	{
		translatePseudoAsmGeneric(i, xi, irb);
		return;
	}
	auto* elemTy = irb.getIntNTy(elemBits);
	auto* vecTy = vecType(elemTy, n);
	Value* vs = irb.CreateBitCast(irb.CreateZExtOrTrunc(src, irb.getIntNTy(destBits)), vecTy);
	Value* amtV = nullptr;
	if (xi->operands[2].type == X86_OP_IMM)
	{
		amtV = irb.CreateVectorSplat(n, ConstantInt::get(elemTy, static_cast<uint64_t>(xi->operands[2].imm)));
	}
	else
	{
		unsigned ab = destBits;
		Value* am = loadVectorOp(xi->operands[2], irb, ab);
		if (am == nullptr)
		{
			translatePseudoAsmGeneric(i, xi, irb);
			return;
		}
		amtV = irb.CreateBitCast(irb.CreateZExtOrTrunc(am, irb.getIntNTy(destBits)), vecTy);
	}
	Value* res = UndefValue::get(vecTy);
	Value* width = ConstantInt::get(elemTy, elemBits);
	for (unsigned lane = 0; lane < n; ++lane)
	{
		Value* v = irb.CreateExtractElement(vs, lane);
		Value* a = irb.CreateExtractElement(amtV, lane);
		Value* out = nullptr;
		if (rot)
		{
			out = rotateLane(v, a, elemBits, irb);
		}
		else
		{
			Value* neg = irb.CreateICmpSLT(a, Constant::getNullValue(elemTy));
			Value* mag = irb.CreateSelect(neg, irb.CreateNeg(a), a);
			Value* tooBig = irb.CreateICmpUGE(mag, width);
			Value* safe = irb.CreateSelect(tooBig, ConstantInt::get(elemTy, elemBits - 1), mag);
			Value* left = irb.CreateSelect(tooBig, Constant::getNullValue(elemTy), irb.CreateShl(v, safe));
			Value* right = arith ? irb.CreateAShr(v, safe)
				: irb.CreateSelect(tooBig, Constant::getNullValue(elemTy), irb.CreateLShr(v, safe));
			out = irb.CreateSelect(neg, right, left);
		}
		res = irb.CreateInsertElement(res, out, lane);
	}
	storeVectorOp(xi->operands[0], irb.CreateBitCast(res, irb.getIntNTy(destBits)), destBits, irb);
}

void Capstone2LlvmIrTranslatorX86_impl::translateRdrand(cs_insn* i, cs_x86* xi, IRBuilder<>& irb)
{
	if (xi->op_count != 1 || xi->operands[0].type != X86_OP_REG)
	{
		translatePseudoAsmGeneric(i, xi, irb);
		return;
	}
	unsigned bits = xi->operands[0].size * 8;
	if (bits != 16 && bits != 32 && bits != 64)
	{
		bits = 32;
	}
	const char* name = (i->id == X86_INS_RDRAND) ? "__asm_rdrand" : "__asm_rdseed";
	auto* fty = FunctionType::get(irb.getIntNTy(bits), false);
	Value* val = irb.CreateCall(_module->getOrInsertFunction(name, fty));
	storeOp(xi->operands[0], val, irb, eOpConv::ZEXT_TRUNC_OR_BITCAST);
	storeRegister(X86_REG_CF, irb.getTrue(), irb);
}

/**
 * The MMX <-> SSE conversions: CVTPI2PD, CVTPI2PS, CVTPS2PI, CVTTPS2PI,
 * CVTPD2PI, CVTTPD2PI. An MMX register is a plain i64 here, so these are
 * the two-lane cases of the SSE packed conversions. Like CVTPS2DQ, the
 * non-T forms round to nearest even and out-of-range lanes give the
 * integer indefinite. CVTPI2PS keeps the upper half of the destination.
 */
void Capstone2LlvmIrTranslatorX86_impl::translateMmxCvt(
        cs_insn* i, cs_x86* xi, IRBuilder<>& irb) {
    EXPECT_IS_BINARY(i, xi, irb);
    Value* src = loadOp(xi->operands[1], irb);
    if (src == nullptr || !src->getType()->isIntegerTy())
    {
        translatePseudoAsmGeneric(i, xi, irb);
        return;
    }
    auto* i32 = irb.getInt32Ty();
    auto* i64 = irb.getInt64Ty();
    auto* i128 = irb.getInt128Ty();
    switch (i->id)
    {
        case X86_INS_CVTPI2PD:
        case X86_INS_CVTPI2PS:
        {
            bool dbl = i->id == X86_INS_CVTPI2PD;
            Value* bits = irb.CreateZExtOrTrunc(src, i64);
            Value* lanes = irb.CreateBitCast(bits, vecType(i32, 2));
            Type* fty = dbl ? irb.getDoubleTy() : irb.getFloatTy();
            Value* f = irb.CreateSIToFP(lanes, vecType(fty, 2));
            if (dbl)
            {
                storeOp(xi->operands[0], irb.CreateBitCast(f, i128), irb, eOpConv::NOTHING);
                return;
            }
            Value* lo = irb.CreateZExt(irb.CreateBitCast(f, i64), i128);
            Value* dst = toI128(loadOp(xi->operands[0], irb), irb);
            Value* hiMask = irb.CreateShl(
                    irb.CreateZExt(ConstantInt::get(i64, ~uint64_t(0)), i128),
                    ConstantInt::get(i128, 64));
            storeOp(xi->operands[0], irb.CreateOr(irb.CreateAnd(dst, hiMask), lo), irb, eOpConv::NOTHING);
            return;
        }
        case X86_INS_CVTPS2PI:
        case X86_INS_CVTTPS2PI:
        case X86_INS_CVTPD2PI:
        case X86_INS_CVTTPD2PI:
        {
            bool dbl = i->id == X86_INS_CVTPD2PI || i->id == X86_INS_CVTTPD2PI;
            bool trunc = i->id == X86_INS_CVTTPS2PI || i->id == X86_INS_CVTTPD2PI;
            Type* fty = dbl ? irb.getDoubleTy() : irb.getFloatTy();
            Value* bits = irb.CreateZExtOrTrunc(src, dbl ? i128 : i64);
            Value* v = irb.CreateBitCast(bits, vecType(fty, 2));
            Value* packed = ConstantInt::get(i64, 0);
            for (unsigned lane = 0; lane < 2; ++lane)
            {
                Value* e = irb.CreateExtractElement(v, (uint64_t)lane);
                if (!trunc)
                {
                    e = irb.CreateUnaryIntrinsic(Intrinsic::roundeven, e);
                }
                Value* w = irb.CreateZExt(generateFpToSiDefined(e, i32, irb), i64);
                if (lane)
                {
                    w = irb.CreateShl(w, ConstantInt::get(i64, 32));
                }
                packed = irb.CreateOr(packed, w);
            }
            storeOp(xi->operands[0], packed, irb, eOpConv::ZEXT_TRUNC_OR_BITCAST);
            return;
        }
        default:
            throw GenericError("Unhandled insn ID in translateMmxCvt().");
    }
}

/**
 * AVX-512 lane-wise bit ops without masking: VPOPCNT{B,W,D,Q}, the
 * rotates VPROL/VPROR (immediate and variable), the funnel shifts
 * VPSHLD/VPSHRD (immediate and VPSHLDV/VPSHRDV variable) and VPTERNLOG.
 * Masked, broadcast or rounding forms stay pseudo calls.
 */
void Capstone2LlvmIrTranslatorX86_impl::translateAvx512BitOps(cs_insn* i, cs_x86* xi, llvm::IRBuilder<>& irb)
{
	enum Kind { POPCNT, ROT_IMM, ROT_VAR, SHD_IMM, SHD_VAR, TERN, PERM, MADD52, DOT, PERMT2, PERMI2, MSHIFT, GFMUL };
	bool hi = false;
	bool sat = false;
	bool words = false;
	Kind kind = POPCNT;
	unsigned eb = 32;
	bool right = false;
	switch (i->id)
	{
	case X86_INS_VPOPCNTB: eb = 8; break;
	case X86_INS_VPOPCNTW: eb = 16; break;
	case X86_INS_VPOPCNTD: eb = 32; break;
	case X86_INS_VPOPCNTQ: eb = 64; break;
	case X86_INS_VPROLD: kind = ROT_IMM; break;
	case X86_INS_VPROLQ: kind = ROT_IMM; eb = 64; break;
	case X86_INS_VPRORD: kind = ROT_IMM; right = true; break;
	case X86_INS_VPRORQ: kind = ROT_IMM; eb = 64; right = true; break;
	case X86_INS_VPROLVD: kind = ROT_VAR; break;
	case X86_INS_VPROLVQ: kind = ROT_VAR; eb = 64; break;
	case X86_INS_VPRORVD: kind = ROT_VAR; right = true; break;
	case X86_INS_VPRORVQ: kind = ROT_VAR; eb = 64; right = true; break;
	case X86_INS_VPSHLDW: kind = SHD_IMM; eb = 16; break;
	case X86_INS_VPSHLDD: kind = SHD_IMM; break;
	case X86_INS_VPSHLDQ: kind = SHD_IMM; eb = 64; break;
	case X86_INS_VPSHRDW: kind = SHD_IMM; eb = 16; right = true; break;
	case X86_INS_VPSHRDD: kind = SHD_IMM; right = true; break;
	case X86_INS_VPSHRDQ: kind = SHD_IMM; eb = 64; right = true; break;
	case X86_INS_VPSHLDVW: kind = SHD_VAR; eb = 16; break;
	case X86_INS_VPSHLDVD: kind = SHD_VAR; break;
	case X86_INS_VPSHLDVQ: kind = SHD_VAR; eb = 64; break;
	case X86_INS_VPSHRDVW: kind = SHD_VAR; eb = 16; right = true; break;
	case X86_INS_VPSHRDVD: kind = SHD_VAR; right = true; break;
	case X86_INS_VPSHRDVQ: kind = SHD_VAR; eb = 64; right = true; break;
	case X86_INS_VPTERNLOGD: kind = TERN; break;
	case X86_INS_VPTERNLOGQ: kind = TERN; eb = 64; break;
	case X86_INS_VPERMB: kind = PERM; eb = 8; break;
	case X86_INS_VPERMW: kind = PERM; eb = 16; break;
	case X86_INS_VPMADD52LUQ: kind = MADD52; eb = 64; break;
	case X86_INS_VPMADD52HUQ: kind = MADD52; eb = 64; hi = true; break;
	case X86_INS_VPDPBUSD: kind = DOT; break;
	case X86_INS_VPDPBUSDS: kind = DOT; sat = true; break;
	case X86_INS_VPDPWSSD: kind = DOT; words = true; break;
	case X86_INS_VPDPWSSDS: kind = DOT; words = true; sat = true; break;
	case X86_INS_VPERMT2B: kind = PERMT2; eb = 8; break;
	case X86_INS_VPERMT2W: kind = PERMT2; eb = 16; break;
	case X86_INS_VPERMI2B: kind = PERMI2; eb = 8; break;
	case X86_INS_VPERMI2W: kind = PERMI2; eb = 16; break;
	case X86_INS_VPMULTISHIFTQB: kind = MSHIFT; eb = 64; break;
	case X86_INS_VGF2P8MULB: kind = GFMUL; eb = 8; break;
	default:
		translatePseudoAsmGeneric(i, xi, irb);
		return;
	}

	unsigned want = 0;
	switch (kind)
	{
	case POPCNT: want = 2; break;
	case ROT_IMM: case ROT_VAR: want = 3; break;
	case SHD_IMM: case TERN: want = 4; break;
	case SHD_VAR: case PERM: case MADD52: case DOT: case PERMT2: case PERMI2: case MSHIFT: case GFMUL: want = 3; break;
	}
	bool bcast = false;
	for (unsigned k = 0; k < xi->op_count; ++k)
	{
		bcast |= xi->operands[k].avx_bcast != X86_AVX_BCAST_INVALID;
	}
	unsigned destBits = xi->op_count ? xi->operands[0].size * 8 : 0;
	bool immLast = kind == ROT_IMM || kind == SHD_IMM || kind == TERN;
	if (xi->op_count != want || hasEvexModifier(xi) || bcast
			|| (destBits != 128 && destBits != 256 && destBits != 512)
			|| (immLast && xi->operands[want - 1].type != X86_OP_IMM))
	{
		translatePseudoAsmGeneric(i, xi, irb);
		return;
	}

	unsigned n = destBits / eb;
	auto* intTy = irb.getIntNTy(destBits);
	auto* elemTy = irb.getIntNTy(eb);
	auto* vecTy = vecType(elemTy, n);
	auto vop = [&](unsigned k) -> llvm::Value* {
		unsigned bits = destBits;
		llvm::Value* v = loadVectorOp(xi->operands[k], irb, bits);
		if (v == nullptr)
		{
			return nullptr;
		}
		return irb.CreateBitCast(irb.CreateZExtOrTrunc(v, intTy), vecTy);
	};
	// The test interpreter only lowers scalar intrinsics, so work lane by lane.
	// Funnel shifts spelled out with plain shifts; the count is taken modulo
	// the lane width and the double shift avoids an out-of-range amount.
	auto funnel = [&](bool r, llvm::Value* hi, llvm::Value* lo, llvm::Value* c) -> llvm::Value* {
		auto* one = llvm::ConstantInt::get(elemTy, 1);
		auto* m = llvm::ConstantInt::get(elemTy, eb - 1);
		c = irb.CreateAnd(c, m);
		auto* inv = irb.CreateSub(m, c);
		if (r)
		{
			return irb.CreateOr(irb.CreateLShr(lo, c), irb.CreateShl(irb.CreateShl(hi, one), inv));
		}
		return irb.CreateOr(irb.CreateShl(hi, c), irb.CreateLShr(irb.CreateLShr(lo, one), inv));
	};
	llvm::Value* v[3] = {nullptr, nullptr, nullptr};
	unsigned first = (kind == SHD_VAR || kind == TERN || kind == MADD52 || kind == DOT || kind == PERMT2 || kind == PERMI2) ? 0 : 1;
	unsigned last = kind == POPCNT || kind == ROT_IMM ? 1 : 2;
	for (unsigned k = first; k <= last; ++k)
	{
		v[k] = vop(k);
		if (v[k] == nullptr)
		{
			translatePseudoAsmGeneric(i, xi, irb);
			return;
		}
	}
	uint64_t imm = immLast ? static_cast<uint64_t>(xi->operands[want - 1].imm) : 0;
	llvm::Value* res = llvm::UndefValue::get(vecTy);
	for (unsigned lane = 0; lane < n; ++lane)
	{
		llvm::Value* e[3] = {nullptr, nullptr, nullptr};
		for (unsigned k = first; k <= last; ++k)
		{
			e[k] = irb.CreateExtractElement(v[k], lane);
		}
		llvm::Value* r = nullptr;
		auto* cImm = llvm::ConstantInt::get(elemTy, imm & (eb - 1));
		switch (kind)
		{
		case POPCNT: r = irb.CreateUnaryIntrinsic(llvm::Intrinsic::ctpop, e[1]); break;
		case ROT_IMM: r = funnel(right, e[1], e[1], cImm); break;
		case ROT_VAR: r = funnel(right, e[1], e[1], e[2]); break;
		case SHD_IMM: r = right ? funnel(true, e[2], e[1], cImm) : funnel(false, e[1], e[2], cImm); break;
		case SHD_VAR: r = right ? funnel(true, e[1], e[0], e[2]) : funnel(false, e[0], e[1], e[2]); break;
		case TERN:
		{
			r = llvm::ConstantInt::get(elemTy, 0);
			for (unsigned k = 0; k < 8; ++k)
			{
				if (!((imm >> k) & 1))
				{
					continue;
				}
				auto* t = irb.CreateAnd(irb.CreateAnd(
						(k & 4) ? e[0] : irb.CreateNot(e[0]),
						(k & 2) ? e[1] : irb.CreateNot(e[1])),
						(k & 1) ? e[2] : irb.CreateNot(e[2]));
				r = irb.CreateOr(r, t);
			}
			break;
		}
		case PERM:
		{
			// Index operand is op1, table op2; only the low log2(n) bits count.
			auto* idx = irb.CreateAnd(e[1], llvm::ConstantInt::get(elemTy, n - 1));
			r = irb.CreateExtractElement(v[2], irb.CreateZExt(idx, irb.getInt32Ty()));
			break;
		}
		case MADD52:
		{
			auto* i128 = irb.getInt128Ty();
			auto* m52 = llvm::ConstantInt::get(i128, (uint64_t(1) << 52) - 1);
			auto* p = irb.CreateMul(
					irb.CreateAnd(irb.CreateZExt(e[1], i128), m52),
					irb.CreateAnd(irb.CreateZExt(e[2], i128), m52));
			p = hi ? irb.CreateLShr(p, 52) : irb.CreateAnd(p, m52);
			r = irb.CreateAdd(e[0], irb.CreateTrunc(p, elemTy));
			break;
		}
		case DOT:
		{
			auto* i64 = irb.getInt64Ty();
			unsigned pb = words ? 16 : 8;
			auto* pty = irb.getIntNTy(pb);
			llvm::Value* sum = irb.CreateSExt(e[0], i64);
			for (unsigned j = 0; j < 32 / pb; ++j)
			{
				auto part = [&](llvm::Value* x, bool sgn) {
					auto* t = irb.CreateTrunc(irb.CreateLShr(x, j * pb), pty);
					return sgn ? irb.CreateSExt(t, i64) : irb.CreateZExt(t, i64);
				};
				sum = irb.CreateAdd(sum, irb.CreateMul(part(e[1], words), part(e[2], true)));
			}
			if (sat)
			{
				auto* mx = llvm::ConstantInt::get(i64, 0x7fffffff);
				auto* mn = llvm::ConstantInt::getSigned(i64, -0x80000000ll);
				sum = irb.CreateSelect(irb.CreateICmpSGT(sum, mx), mx, sum);
				sum = irb.CreateSelect(irb.CreateICmpSLT(sum, mn), mn, sum);
			}
			r = irb.CreateTrunc(sum, elemTy);
			break;
		}
		case PERMT2:
		case PERMI2:
		{
			// Two-table permute: bit log2(n) of the index picks the table.
			// T2: index op1, tables op0 (overwritten) and op2.
			// I2: index op0 (overwritten), tables op1 and op2.
			llvm::Value* ix = kind == PERMT2 ? e[1] : e[0];
			llvm::Value* t1 = kind == PERMT2 ? v[0] : v[1];
			auto* i32 = irb.getInt32Ty();
			auto* idx = irb.CreateZExt(irb.CreateAnd(ix, llvm::ConstantInt::get(elemTy, n - 1)), i32);
			auto* sel = irb.CreateICmpNE(irb.CreateAnd(ix, llvm::ConstantInt::get(elemTy, n)),
					llvm::ConstantInt::get(elemTy, 0));
			r = irb.CreateSelect(sel, irb.CreateExtractElement(v[2], idx), irb.CreateExtractElement(t1, idx));
			break;
		}
		case MSHIFT:
		{
			// Each control byte picks an unaligned byte (rotating) from the data qword.
			auto* i64 = irb.getInt64Ty();
			r = llvm::ConstantInt::get(i64, 0);
			for (unsigned j = 0; j < 8; ++j)
			{
				auto* c = irb.CreateAnd(irb.CreateLShr(e[1], 8 * j), llvm::ConstantInt::get(i64, 63));
				auto* inv = irb.CreateAnd(irb.CreateSub(llvm::ConstantInt::get(i64, 64), c),
						llvm::ConstantInt::get(i64, 63));
				auto* rot = irb.CreateOr(irb.CreateLShr(e[2], c), irb.CreateShl(e[2], inv));
				r = irb.CreateOr(r, irb.CreateShl(irb.CreateAnd(rot, llvm::ConstantInt::get(i64, 0xff)), 8 * j));
			}
			break;
		}
		case GFMUL:
		{
			// GF(2^8) multiply modulo x^8 + x^4 + x^3 + x + 1.
			auto* i16 = irb.getInt16Ty();
			llvm::Value* a = irb.CreateZExt(e[1], i16);
			llvm::Value* b = irb.CreateZExt(e[2], i16);
			llvm::Value* acc = llvm::ConstantInt::get(i16, 0);
			for (unsigned j = 0; j < 8; ++j)
			{
				auto* bit = irb.CreateAnd(irb.CreateLShr(b, j), llvm::ConstantInt::get(i16, 1));
				acc = irb.CreateXor(acc, irb.CreateMul(irb.CreateShl(a, j), bit));
			}
			for (int j = 14; j >= 8; --j)
			{
				auto* bit = irb.CreateAnd(irb.CreateLShr(acc, j), llvm::ConstantInt::get(i16, 1));
				acc = irb.CreateXor(acc, irb.CreateMul(llvm::ConstantInt::get(i16, 0x11b << (j - 8)), bit));
			}
			r = irb.CreateTrunc(acc, elemTy);
			break;
		}
		}
		res = irb.CreateInsertElement(res, r, lane);
	}
	storeVectorOp(xi->operands[0], irb.CreateBitCast(res, intTy), destBits, irb);
}


void Capstone2LlvmIrTranslatorX86_impl::translateAvxChunks(cs_insn* i, cs_x86* xi, llvm::IRBuilder<>& irb)
{
	// Whole-chunk moves: broadcasts of a 64/128/256-bit block, 128-bit lane
	// shuffles, extracts of 128/256-bit halves, and XOP horizontal add/sub.
	enum Kind { BCAST, SHUF, EXTRACT, HADD, HSUB, MACS };
	Kind kind = BCAST;
	unsigned sb = 0, db = 0;
	bool sgn = true;
	bool hiHalf = false;
	switch (i->id)
	{
	case X86_INS_VBROADCASTF32X2: case X86_INS_VBROADCASTI32X2: sb = 64; break;
	case X86_INS_VBROADCASTF32X4: case X86_INS_VBROADCASTF64X2:
	case X86_INS_VBROADCASTI64X2: case X86_INS_VBROADCASTI128: sb = 128; break;
	case X86_INS_VBROADCASTF32X8: case X86_INS_VBROADCASTF64X4:
	case X86_INS_VBROADCASTI32X8: sb = 256; break;
	case X86_INS_VSHUFF32X4: case X86_INS_VSHUFF64X2:
	case X86_INS_VSHUFI32X4: case X86_INS_VSHUFI64X2: kind = SHUF; break;
	case X86_INS_VEXTRACTF32X8: case X86_INS_VEXTRACTI32X8:
	case X86_INS_VEXTRACTF64X2: case X86_INS_VEXTRACTI64X2: kind = EXTRACT; break;
	case X86_INS_VPHADDBW: kind = HADD; sb = 8; db = 16; break;
	case X86_INS_VPHADDBD: kind = HADD; sb = 8; db = 32; break;
	case X86_INS_VPHADDBQ: kind = HADD; sb = 8; db = 64; break;
	case X86_INS_VPHADDWD: kind = HADD; sb = 16; db = 32; break;
	case X86_INS_VPHADDWQ: kind = HADD; sb = 16; db = 64; break;
	case X86_INS_VPHADDDQ: kind = HADD; sb = 32; db = 64; break;
	case X86_INS_VPHADDUBW: kind = HADD; sb = 8; db = 16; sgn = false; break;
	case X86_INS_VPHADDUBD: kind = HADD; sb = 8; db = 32; sgn = false; break;
	case X86_INS_VPHADDUBQ: kind = HADD; sb = 8; db = 64; sgn = false; break;
	case X86_INS_VPHADDUWD: kind = HADD; sb = 16; db = 32; sgn = false; break;
	case X86_INS_VPHADDUWQ: kind = HADD; sb = 16; db = 64; sgn = false; break;
	case X86_INS_VPHADDUDQ: kind = HADD; sb = 32; db = 64; sgn = false; break;
	case X86_INS_VPHSUBBW: kind = HSUB; sb = 8; db = 16; break;
	case X86_INS_VPHSUBWD: kind = HSUB; sb = 16; db = 32; break;
	case X86_INS_VPHSUBDQ: kind = HSUB; sb = 32; db = 64; break;
	// XOP multiply-accumulate: dst = src1 * src2 + src3 per lane; db is the
	// accumulator lane, sb the factor lane (DQL/DQH pick the low/high dword).
	case X86_INS_VPMACSWW: kind = MACS; sb = 16; db = 16; sgn = false; break;
	case X86_INS_VPMACSSWW: kind = MACS; sb = 16; db = 16; break;
	case X86_INS_VPMACSDD: kind = MACS; sb = 32; db = 32; sgn = false; break;
	case X86_INS_VPMACSSDD: kind = MACS; sb = 32; db = 32; break;
	case X86_INS_VPMACSDQL: kind = MACS; sb = 32; db = 64; sgn = false; break;
	case X86_INS_VPMACSSDQL: kind = MACS; sb = 32; db = 64; break;
	case X86_INS_VPMACSDQH: kind = MACS; sb = 32; db = 64; sgn = false; hiHalf = true; break;
	case X86_INS_VPMACSSDQH: kind = MACS; sb = 32; db = 64; hiHalf = true; break;
	default:
		translatePseudoAsmGeneric(i, xi, irb);
		return;
	}
	unsigned want = kind == SHUF || kind == MACS ? 4 : (kind == EXTRACT ? 3 : 2);
	bool bcast = false;
	for (unsigned k = 0; k < xi->op_count; ++k)
	{
		bcast |= xi->operands[k].avx_bcast != X86_AVX_BCAST_INVALID;
	}
	if (xi->op_count != want || hasEvexModifier(xi) || bcast
			|| (want > 2 && kind != MACS && xi->operands[want - 1].type != X86_OP_IMM))
	{
		translatePseudoAsmGeneric(i, xi, irb);
		return;
	}
	unsigned destBits = xi->operands[0].size * 8;
	auto load = [&](unsigned k, unsigned bits) -> llvm::Value* {
		unsigned b = bits;
		auto* v = loadVectorOp(xi->operands[k], irb, b);
		if (v == nullptr) return nullptr;
		return irb.CreateZExtOrTrunc(v, irb.getIntNTy(bits));
	};
	auto chunk = [&](llvm::Value* v, unsigned idx, unsigned bits) {
		auto* t = v->getType();
		return irb.CreateTrunc(irb.CreateLShr(v, llvm::ConstantInt::get(t, idx * bits)), irb.getIntNTy(bits));
	};
	auto* dt = irb.getIntNTy(destBits ? destBits : 8);
	llvm::Value* res = nullptr;
	switch (kind)
	{
	case BCAST:
	{
		// The source register may be wider than the block; take its low bits.
		unsigned srcBits = xi->operands[1].type == X86_OP_MEM ? xi->operands[1].size * 8 : 0;
		if (srcBits == 0)
		{
			unsigned b = 0;
			auto* v = loadVectorOp(xi->operands[1], irb, b);
			if (v == nullptr || destBits <= sb) { translatePseudoAsmGeneric(i, xi, irb); return; }
			res = irb.CreateTrunc(v, irb.getIntNTy(sb));
		}
		else
		{
			if (srcBits != sb || destBits <= sb) { translatePseudoAsmGeneric(i, xi, irb); return; }
			res = load(1, sb);
		}
		if (res == nullptr) { translatePseudoAsmGeneric(i, xi, irb); return; }
		auto* w = irb.CreateZExt(res, dt);
		llvm::Value* acc = llvm::ConstantInt::get(dt, 0);
		for (unsigned k = 0; k < destBits / sb; ++k)
		{
			acc = irb.CreateOr(acc, irb.CreateShl(w, llvm::ConstantInt::get(dt, k * sb)));
		}
		res = acc;
		break;
	}
	case SHUF:
	{
		if (destBits != 256 && destBits != 512) { translatePseudoAsmGeneric(i, xi, irb); return; }
		auto* a = load(1, destBits);
		auto* b = load(2, destBits);
		if (a == nullptr || b == nullptr) { translatePseudoAsmGeneric(i, xi, irb); return; }
		unsigned imm = unsigned(xi->operands[3].imm);
		unsigned n = destBits / 128, bitsPer = n == 4 ? 2 : 1;
		res = llvm::ConstantInt::get(dt, 0);
		for (unsigned k = 0; k < n; ++k)
		{
			unsigned sel = (imm >> (k * bitsPer)) & ((1u << bitsPer) - 1);
			auto* c = chunk(k < n / 2 ? a : b, sel, 128);
			res = irb.CreateOr(res, irb.CreateShl(irb.CreateZExt(c, dt), llvm::ConstantInt::get(dt, k * 128)));
		}
		break;
	}
	case EXTRACT:
	{
		unsigned srcBits = xi->operands[1].size * 8;
		if (destBits == 0 || srcBits <= destBits) { translatePseudoAsmGeneric(i, xi, irb); return; }
		auto* a = load(1, srcBits);
		if (a == nullptr) { translatePseudoAsmGeneric(i, xi, irb); return; }
		unsigned sel = unsigned(xi->operands[2].imm) & (srcBits / destBits - 1);
		res = chunk(a, sel, destBits);
		break;
	}
	case HADD:
	case HSUB:
	{
		if (destBits != 128) { translatePseudoAsmGeneric(i, xi, irb); return; }
		auto* a = load(1, 128);
		if (a == nullptr) { translatePseudoAsmGeneric(i, xi, irb); return; }
		auto* et = irb.getIntNTy(db);
		res = llvm::ConstantInt::get(dt, 0);
		unsigned per = db / sb;
		for (unsigned k = 0; k < 128 / db; ++k)
		{
			llvm::Value* s = nullptr;
			if (kind == HSUB)
			{
				s = irb.CreateSub(irb.CreateSExt(chunk(a, 2 * k, sb), et), irb.CreateSExt(chunk(a, 2 * k + 1, sb), et));
			}
			else
			{
				s = llvm::ConstantInt::get(et, 0);
				for (unsigned j = 0; j < per; ++j)
				{
					auto* e = chunk(a, k * per + j, sb);
					s = irb.CreateAdd(s, sgn ? irb.CreateSExt(e, et) : irb.CreateZExt(e, et));
				}
			}
			res = irb.CreateOr(res, irb.CreateShl(irb.CreateZExt(s, dt), llvm::ConstantInt::get(dt, k * db)));
		}
		break;
	}
	case MACS:
	{
		// sgn here means "saturate"; the products are always signed.
		if (destBits != 128) { translatePseudoAsmGeneric(i, xi, irb); return; }
		auto* a = load(1, 128);
		auto* b = load(2, 128);
		auto* c = load(3, 128);
		if (a == nullptr || b == nullptr || c == nullptr) { translatePseudoAsmGeneric(i, xi, irb); return; }
		auto* wt = irb.getIntNTy(db * 2);
		auto* et = irb.getIntNTy(db);
		res = llvm::ConstantInt::get(dt, 0);
		for (unsigned k = 0; k < 128 / db; ++k)
		{
			unsigned fi = db == sb ? k : 2 * k + (hiHalf ? 1 : 0);
			auto* p = irb.CreateMul(irb.CreateSExt(chunk(a, fi, sb), wt), irb.CreateSExt(chunk(b, fi, sb), wt));
			llvm::Value* s = irb.CreateAdd(p, irb.CreateSExt(chunk(c, k, db), wt));
			if (sgn)
			{
				auto* mx = llvm::ConstantInt::get(wt, llvm::APInt::getSignedMaxValue(db).sext(db * 2));
				auto* mn = llvm::ConstantInt::get(wt, llvm::APInt::getSignedMinValue(db).sext(db * 2));
				s = irb.CreateSelect(irb.CreateICmpSGT(s, mx), mx, s);
				s = irb.CreateSelect(irb.CreateICmpSLT(s, mn), mn, s);
			}
			s = irb.CreateTrunc(s, et);
			res = irb.CreateOr(res, irb.CreateShl(irb.CreateZExt(s, dt), llvm::ConstantInt::get(dt, k * db)));
		}
		break;
	}
	}
	if (xi->operands[0].type == X86_OP_MEM)
	{
		storeOp(xi->operands[0], res, irb);
		return;
	}
	storeVectorOp(xi->operands[0], res, destBits, irb);
}


void Capstone2LlvmIrTranslatorX86_impl::translateAvxCvtQq(cs_insn* i, cs_x86* xi, llvm::IRBuilder<>& irb)
{
	// AVX512DQ 64-bit integer <-> float/double conversions, lane by lane.
	// Rounding follows round-to-nearest-even for the non-T forms (MXCSR not modelled).
	bool toInt = true, trunc = false, uns = false, dbl = true;
	switch (i->id)
	{
	case X86_INS_VCVTTPD2QQ: trunc = true; break;
	case X86_INS_VCVTPD2QQ: break;
	case X86_INS_VCVTTPD2UQQ: trunc = true; uns = true; break;
	case X86_INS_VCVTPD2UQQ: uns = true; break;
	case X86_INS_VCVTTPS2QQ: trunc = true; dbl = false; break;
	case X86_INS_VCVTPS2QQ: dbl = false; break;
	case X86_INS_VCVTTPS2UQQ: trunc = true; uns = true; dbl = false; break;
	case X86_INS_VCVTPS2UQQ: uns = true; dbl = false; break;
	case X86_INS_VCVTQQ2PD: toInt = false; break;
	case X86_INS_VCVTUQQ2PD: toInt = false; uns = true; break;
	case X86_INS_VCVTQQ2PS: toInt = false; dbl = false; break;
	case X86_INS_VCVTUQQ2PS: toInt = false; uns = true; dbl = false; break;
	default: translatePseudoAsmGeneric(i, xi, irb); return;
	}
	bool bcast = false;
	for (unsigned k = 0; k < xi->op_count; ++k)
	{
		bcast |= xi->operands[k].avx_bcast != X86_AVX_BCAST_INVALID;
	}
	if (xi->op_count != 2 || hasEvexModifier(xi) || bcast)
	{
		translatePseudoAsmGeneric(i, xi, irb);
		return;
	}
	auto* fpTy = dbl ? irb.getDoubleTy() : irb.getFloatTy();
	auto* i64 = irb.getInt64Ty();
	unsigned fb = dbl ? 64 : 32;
	unsigned srcElem = toInt ? fb : 64, dstElem = toInt ? 64 : fb;
	unsigned n = dstElem >= srcElem ? xi->operands[0].size * 8 / dstElem : xi->operands[1].size * 8 / srcElem;
	unsigned srcBits = n * srcElem, destBits = n * dstElem;
	if (n == 0 || (destBits != 64 && destBits != 128 && destBits != 256 && destBits != 512))
	{
		translatePseudoAsmGeneric(i, xi, irb);
		return;
	}
	llvm::Value* raw = nullptr;
	if (xi->operands[1].type == X86_OP_MEM)
	{
		if (xi->operands[1].size * 8 != srcBits) { translatePseudoAsmGeneric(i, xi, irb); return; }
		raw = loadOp(xi->operands[1], irb, irb.getIntNTy(srcBits));
	}
	else
	{
		unsigned b = 0;
		raw = loadVectorOp(xi->operands[1], irb, b);
		if (raw == nullptr || b < srcBits) { translatePseudoAsmGeneric(i, xi, irb); return; }
		raw = irb.CreateTrunc(raw, irb.getIntNTy(srcBits));
	}
	auto* dt = irb.getIntNTy(destBits);
	llvm::Value* res = llvm::ConstantInt::get(dt, 0);
	for (unsigned k = 0; k < n; ++k)
	{
		auto* e = irb.CreateTrunc(irb.CreateLShr(raw, k * srcElem), irb.getIntNTy(srcElem));
		llvm::Value* r = nullptr;
		if (toInt)
		{
			llvm::Value* f = irb.CreateBitCast(e, fpTy);
			if (!trunc)
			{
				// Ties-to-even without intrinsics: (|f| + 2^p) - 2^p for |f| < 2^p.
				auto* m = llvm::ConstantFP::get(fpTy, dbl ? 4503599627370496.0 : 8388608.0);
				auto* neg = irb.CreateFCmpOLT(f, llvm::ConstantFP::get(fpTy, 0.0));
				auto* a = irb.CreateSelect(neg, irb.CreateFNeg(f), f);
				auto* r = irb.CreateFSub(irb.CreateFAdd(a, m), m);
				r = irb.CreateSelect(irb.CreateFCmpOGE(a, m), a, r);
				f = irb.CreateSelect(neg, irb.CreateFNeg(r), r);
			}
			r = uns ? fpToUiDefined(f, i64, irb) : irb.CreateFPToSI(f, i64);
		}
		else
		{
			auto* f = uns ? irb.CreateUIToFP(e, fpTy) : irb.CreateSIToFP(e, fpTy);
			r = irb.CreateBitCast(f, irb.getIntNTy(fb));
		}
		res = irb.CreateOr(res, irb.CreateShl(irb.CreateZExt(r, dt), k * dstElem));
	}
	storeVectorOp(xi->operands[0], destBits < 128 ? irb.CreateZExt(res, irb.getInt128Ty()) : res,
			destBits < 128 ? 128 : destBits, irb);
}


} // namespace capstone2llvmir
} // namespace retdec
