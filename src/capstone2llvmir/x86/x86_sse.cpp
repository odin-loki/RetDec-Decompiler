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
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Type.h>
#include <llvm/IR/Value.h>

// Include the impl header which declares all the translate* functions we define here.
// (The actual class is Capstone2LlvmIrTranslatorX86_impl.)
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
    return VectorType::get(elem, n);
}

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
 * MOVDDUP — duplicate low 64-bit double to both lanes of XMM.
 */
void Capstone2LlvmIrTranslatorX86_impl::translateMovDdup(
        cs_insn* i, cs_x86* xi, IRBuilder<>& irb) {
    EXPECT_IS_BINARY(i, xi, irb);
    auto* src = loadOpBinaryOp1(xi, irb);
    // Grab lower 64 bits as integer, duplicate to upper.
    Value* lo64 = irb.CreateTrunc(src, irb.getInt64Ty());
    Value* lo128 = irb.CreateZExt(lo64, irb.getInt128Ty());
    Value* hi128 = irb.CreateShl(lo128, ConstantInt::get(irb.getInt128Ty(), 64));
    Value* result = irb.CreateOr(lo128, hi128);
    storeOp(xi->operands[0], result, irb, eOpConv::NOTHING);
}

/**
 * MOVSHDUP — duplicate odd (high) 32-bit floats: src[1]→[0,1], src[3]→[2,3].
 * MOVSLDUP — duplicate even (low) 32-bit floats: src[0]→[0,1], src[2]→[2,3].
 */
void Capstone2LlvmIrTranslatorX86_impl::translateMovShDup(
        cs_insn* i, cs_x86* xi, IRBuilder<>& irb) {
    EXPECT_IS_BINARY(i, xi, irb);
    auto* src = loadOpBinaryOp1(xi, irb);
    auto* vec4f = vecType(irb.getFloatTy(), 4);
    auto* v = asVec(src, vec4f, irb);

    bool isHigh = (i->id == X86_INS_MOVSHDUP);
    unsigned lane0 = isHigh ? 1 : 0;
    unsigned lane1 = isHigh ? 3 : 2;

    // Build shuffle: [lane0, lane0, lane1, lane1]
    std::vector<uint32_t> mask = {
        static_cast<uint32_t>(lane0),
        static_cast<uint32_t>(lane0),
        static_cast<uint32_t>(lane1),
        static_cast<uint32_t>(lane1)
    };
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

/// Generic packed integer binary op helper.
static Value* packedIntBinOp(
        Value* op0, Value* op1,
        unsigned elemBits, unsigned numElems,
        unsigned opcode,   // llvm::Instruction::Add etc.
        IRBuilder<>& irb) {
    auto* vecTy = VectorType::get(irb.getIntNTy(elemBits), numElems);
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
 * PCMPEQB/W/D — packed compare equal; result lane is all-1s or all-0s.
 */
void Capstone2LlvmIrTranslatorX86_impl::translateSsePcmpeq(
        cs_insn* i, cs_x86* xi, IRBuilder<>& irb) {
    EXPECT_IS_BINARY(i, xi, irb);
    std::tie(op0, op1) = loadOpBinary(xi, irb, eOpConv::NOTHING);

    unsigned bits;
    switch (i->id) {
        case X86_INS_PCMPEQB: bits =  8; break;
        case X86_INS_PCMPEQW: bits = 16; break;
        case X86_INS_PCMPEQD: bits = 32; break;
        default: bits = 32; break;
    }
    unsigned lanes = 128 / bits;
    auto* vecTy = VectorType::get(irb.getIntNTy(bits), lanes);
    Value* v0 = irb.CreateBitCast(toI128(op0, irb), vecTy);
    Value* v1 = irb.CreateBitCast(toI128(op1, irb), vecTy);
    // i1 vector comparison.
    Value* cmp = irb.CreateICmpEQ(v0, v1);
    // Sign-extend i1 → iN to get 0xFF…F or 0x00…0.
    Value* ext = irb.CreateSExt(cmp, vecTy);
    storeOp(xi->operands[0], irb.CreateBitCast(ext, irb.getInt128Ty()),
            irb, eOpConv::NOTHING);
}

/**
 * PUNPCKLBW — unpack and interleave low bytes.
 * PUNPCKLDQ — unpack and interleave low doublewords.
 */
void Capstone2LlvmIrTranslatorX86_impl::translateSsePunpckl(
        cs_insn* i, cs_x86* xi, IRBuilder<>& irb) {
    EXPECT_IS_BINARY(i, xi, irb);
    std::tie(op0, op1) = loadOpBinary(xi, irb, eOpConv::NOTHING);

    unsigned bits = (i->id == X86_INS_PUNPCKLBW) ? 8 : 32;
    unsigned lanes = 128 / bits;
    auto* vecTy = VectorType::get(irb.getIntNTy(bits), lanes);
    Value* v0 = irb.CreateBitCast(toI128(op0, irb), vecTy);
    Value* v1 = irb.CreateBitCast(toI128(op1, irb), vecTy);

    // Interleave low half: [v0[0], v1[0], v0[1], v1[1], ...]
    unsigned halfLanes = lanes / 2;
    std::vector<uint32_t> mask;
    mask.reserve(lanes);
    for (unsigned n = 0; n < halfLanes; ++n) {
        mask.push_back(static_cast<uint32_t>(n));
        mask.push_back(static_cast<uint32_t>(lanes + n));
    }
    Value* result = irb.CreateShuffleVector(v0, v1, mask);
    storeOp(xi->operands[0], irb.CreateBitCast(result, irb.getInt128Ty()),
            irb, eOpConv::NOTHING);
}

/**
 * PSHUFD — shuffle 32-bit lanes of XMM by immediate control byte.
 * dst[i] = src[ (imm >> (i*2)) & 3 ]
 */
void Capstone2LlvmIrTranslatorX86_impl::translateSsePshufd(
        cs_insn* i, cs_x86* xi, IRBuilder<>& irb) {
    EXPECT_IS_TERNARY(i, xi, irb);
    auto* src = loadOp(xi->operands[1], irb);
    uint8_t ctrl = static_cast<uint8_t>(xi->operands[2].imm);

    auto* vec4i = vecType(irb.getInt32Ty(), 4);
    Value* v = irb.CreateBitCast(src, vec4i);

    std::vector<uint32_t> mask = {
        static_cast<uint32_t>((ctrl >> 0) & 3),
        static_cast<uint32_t>((ctrl >> 2) & 3),
        static_cast<uint32_t>((ctrl >> 4) & 3),
        static_cast<uint32_t>((ctrl >> 6) & 3)
    };
    Value* shuffled = irb.CreateShuffleVector(v, v, mask);
    storeOp(xi->operands[0], irb.CreateBitCast(shuffled, irb.getInt128Ty()),
            irb, eOpConv::NOTHING);
}

/**
 * PSLLDQ — shift XMM left by N bytes (zero-fill from right).
 * PSRLDQ — shift XMM right by N bytes (zero-fill from left).
 */
void Capstone2LlvmIrTranslatorX86_impl::translateSsePbyteShift(
        cs_insn* i, cs_x86* xi, IRBuilder<>& irb) {
    EXPECT_IS_BINARY(i, xi, irb);
    auto* src = loadOpBinaryOp0(xi, irb);
    uint64_t bytes = static_cast<uint64_t>(xi->operands[1].imm);
    uint64_t bits  = bytes * 8;

    Value* result;
    if (bits >= 128) {
        result = ConstantInt::get(irb.getInt128Ty(), 0);
    } else if (i->id == X86_INS_PSLLDQ) {
        result = irb.CreateShl(src, ConstantInt::get(irb.getInt128Ty(), bits));
    } else {
        result = irb.CreateLShr(src, ConstantInt::get(irb.getInt128Ty(), bits));
    }
    storeOp(xi->operands[0], result, irb, eOpConv::NOTHING);
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
    auto* vecTy  = VectorType::get(elemTy, n);
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
    auto* vecTy  = VectorType::get(elemTy, n);
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

/**
 * ADDPS — packed single-precision float add.
 * ADDSS — scalar single-precision float add (lane 0 only).
 * ADDSUBPS — alternating add/sub: even lanes subtract, odd lanes add.
 * HADDPS — horizontal add: dst[0]=src1[0]+src1[1], dst[1]=src1[2]+src1[3],
 *           dst[2]=src2[0]+src2[1], dst[3]=src2[2]+src2[3].
 * HSUBPS — horizontal sub.
 */
void Capstone2LlvmIrTranslatorX86_impl::translateSseAddPs(
        cs_insn* i, cs_x86* xi, IRBuilder<>& irb) {
    EXPECT_IS_BINARY(i, xi, irb);
    std::tie(op0, op1) = loadOpBinary(xi, irb, eOpConv::NOTHING);

    if (i->id == X86_INS_ADDSS) {
        storeOp(xi->operands[0],
                scalarFltBinOp(op0, op1, false, Instruction::FAdd, irb),
                irb, eOpConv::NOTHING);
    } else if (i->id == X86_INS_ADDPS) {
        storeOp(xi->operands[0],
                packedFltBinOp(op0, op1, false, Instruction::FAdd, irb),
                irb, eOpConv::NOTHING);
    } else if (i->id == X86_INS_ADDSUBPS) {
        // Even lanes: subtract; odd lanes: add.
        auto* vec4f = vecType(irb.getFloatTy(), 4);
        Value* v0 = irb.CreateBitCast(toI128(op0, irb), vec4f);
        Value* v1 = irb.CreateBitCast(toI128(op1, irb), vec4f);
        Value* adds = irb.CreateFAdd(v0, v1);
        Value* subs = irb.CreateFSub(v0, v1);
        // Blend: even from subs, odd from adds.
        std::vector<uint32_t> mask = {4, 1, 6, 3}; // subs[0], adds[1], subs[2], adds[3]
        Value* blended = irb.CreateShuffleVector(subs, adds, mask);
        storeOp(xi->operands[0], irb.CreateBitCast(blended, irb.getInt128Ty()),
                irb, eOpConv::NOTHING);
    } else if (i->id == X86_INS_HADDPS) {
        auto* vec4f = vecType(irb.getFloatTy(), 4);
        Value* v0 = irb.CreateBitCast(toI128(op0, irb), vec4f);
        Value* v1 = irb.CreateBitCast(toI128(op1, irb), vec4f);
        Value* e00 = irb.CreateExtractElement(v0, (uint64_t)0);
        Value* e01 = irb.CreateExtractElement(v0, (uint64_t)1);
        Value* e02 = irb.CreateExtractElement(v0, (uint64_t)2);
        Value* e03 = irb.CreateExtractElement(v0, (uint64_t)3);
        Value* e10 = irb.CreateExtractElement(v1, (uint64_t)0);
        Value* e11 = irb.CreateExtractElement(v1, (uint64_t)1);
        Value* e12 = irb.CreateExtractElement(v1, (uint64_t)2);
        Value* e13 = irb.CreateExtractElement(v1, (uint64_t)3);
        Value* res = UndefValue::get(vec4f);
        res = irb.CreateInsertElement(res, irb.CreateFAdd(e00, e01), (uint64_t)0);
        res = irb.CreateInsertElement(res, irb.CreateFAdd(e02, e03), (uint64_t)1);
        res = irb.CreateInsertElement(res, irb.CreateFAdd(e10, e11), (uint64_t)2);
        res = irb.CreateInsertElement(res, irb.CreateFAdd(e12, e13), (uint64_t)3);
        storeOp(xi->operands[0], irb.CreateBitCast(res, irb.getInt128Ty()),
                irb, eOpConv::NOTHING);
    } else if (i->id == X86_INS_HSUBPS) {
        auto* vec4f = vecType(irb.getFloatTy(), 4);
        Value* v0 = irb.CreateBitCast(toI128(op0, irb), vec4f);
        Value* v1 = irb.CreateBitCast(toI128(op1, irb), vec4f);
        Value* e00 = irb.CreateExtractElement(v0, (uint64_t)0);
        Value* e01 = irb.CreateExtractElement(v0, (uint64_t)1);
        Value* e02 = irb.CreateExtractElement(v0, (uint64_t)2);
        Value* e03 = irb.CreateExtractElement(v0, (uint64_t)3);
        Value* e10 = irb.CreateExtractElement(v1, (uint64_t)0);
        Value* e11 = irb.CreateExtractElement(v1, (uint64_t)1);
        Value* e12 = irb.CreateExtractElement(v1, (uint64_t)2);
        Value* e13 = irb.CreateExtractElement(v1, (uint64_t)3);
        Value* res = UndefValue::get(vec4f);
        res = irb.CreateInsertElement(res, irb.CreateFSub(e00, e01), (uint64_t)0);
        res = irb.CreateInsertElement(res, irb.CreateFSub(e02, e03), (uint64_t)1);
        res = irb.CreateInsertElement(res, irb.CreateFSub(e10, e11), (uint64_t)2);
        res = irb.CreateInsertElement(res, irb.CreateFSub(e12, e13), (uint64_t)3);
        storeOp(xi->operands[0], irb.CreateBitCast(res, irb.getInt128Ty()),
                irb, eOpConv::NOTHING);
    }
}

/**
 * MULPS — packed float multiply.
 * MULSS — scalar float multiply (lane 0).
 */
void Capstone2LlvmIrTranslatorX86_impl::translateSseMulPs(
        cs_insn* i, cs_x86* xi, IRBuilder<>& irb) {
    EXPECT_IS_BINARY(i, xi, irb);
    std::tie(op0, op1) = loadOpBinary(xi, irb, eOpConv::NOTHING);
    bool scalar = (i->id == X86_INS_MULSS);
    auto fn = scalar
        ? scalarFltBinOp(op0, op1, false, Instruction::FMul, irb)
        : packedFltBinOp(op0, op1, false, Instruction::FMul, irb);
    storeOp(xi->operands[0], fn, irb, eOpConv::NOTHING);
}

/**
 * DIVPS — packed float divide.
 * DIVSS — scalar float divide (lane 0).
 */
void Capstone2LlvmIrTranslatorX86_impl::translateSseDivPs(
        cs_insn* i, cs_x86* xi, IRBuilder<>& irb) {
    EXPECT_IS_BINARY(i, xi, irb);
    std::tie(op0, op1) = loadOpBinary(xi, irb, eOpConv::NOTHING);
    bool scalar = (i->id == X86_INS_DIVSS);
    auto fn = scalar
        ? scalarFltBinOp(op0, op1, false, Instruction::FDiv, irb)
        : packedFltBinOp(op0, op1, false, Instruction::FDiv, irb);
    storeOp(xi->operands[0], fn, irb, eOpConv::NOTHING);
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
 */
void Capstone2LlvmIrTranslatorX86_impl::translateCvtSs2Si(
        cs_insn* i, cs_x86* xi, IRBuilder<>& irb) {
    EXPECT_IS_BINARY(i, xi, irb);
    auto* src = loadOpBinaryOp1(xi, irb);
    auto* vec4f = vecType(irb.getFloatTy(), 4);
    Value* v = irb.CreateBitCast(toI128(src, irb), vec4f);
    Value* lane0 = irb.CreateExtractElement(v, (uint64_t)0);
    // Round toward nearest (C default); use FPToSI (truncation).
    unsigned destBits = _basicMode == CS_MODE_64 ? 64 : 32;
    Value* intVal = irb.CreateFPToSI(lane0, irb.getIntNTy(destBits));
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
 * CVTSD2SI — float64 lane 0 → int32/64.
 */
void Capstone2LlvmIrTranslatorX86_impl::translateCvtSd2Si(
        cs_insn* i, cs_x86* xi, IRBuilder<>& irb) {
    EXPECT_IS_BINARY(i, xi, irb);
    auto* src = loadOpBinaryOp1(xi, irb);
    auto* vec2d = vecType(irb.getDoubleTy(), 2);
    Value* v = irb.CreateBitCast(toI128(src, irb), vec2d);
    Value* lane0 = irb.CreateExtractElement(v, (uint64_t)0);
    unsigned destBits = _basicMode == CS_MODE_64 ? 64 : 32;
    Value* intVal = irb.CreateFPToSI(lane0, irb.getIntNTy(destBits));
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
    auto* src = loadOpBinaryOp1(xi, irb);
    auto* vec4i = vecType(irb.getInt32Ty(), 4);
    auto* vec4f = vecType(irb.getFloatTy(), 4);
    Value* v    = irb.CreateBitCast(toI128(src, irb), vec4i);
    Value* res  = irb.CreateSIToFP(v, vec4f);
    storeOp(xi->operands[0], irb.CreateBitCast(res, irb.getInt128Ty()),
            irb, eOpConv::NOTHING);
}

/**
 * CVTPS2DQ — packed float32 → packed int32 (truncation).
 */
void Capstone2LlvmIrTranslatorX86_impl::translateCvtPs2Dq(
        cs_insn* i, cs_x86* xi, IRBuilder<>& irb) {
    EXPECT_IS_BINARY(i, xi, irb);
    auto* src = loadOpBinaryOp1(xi, irb);
    auto* vec4f = vecType(irb.getFloatTy(), 4);
    auto* vec4i = vecType(irb.getInt32Ty(), 4);
    Value* v   = irb.CreateBitCast(toI128(src, irb), vec4f);
    Value* res = irb.CreateFPToSI(v, vec4i);
    storeOp(xi->operands[0], irb.CreateBitCast(res, irb.getInt128Ty()),
            irb, eOpConv::NOTHING);
}

} // namespace capstone2llvmir
} // namespace retdec
