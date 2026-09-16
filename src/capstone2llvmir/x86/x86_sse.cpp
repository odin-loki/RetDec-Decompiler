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
    return FixedVectorType::get(elem, n);
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
    std::vector<int> mask = {
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
    EXPECT_IS_BINARY(i, xi, irb);
    std::tie(op0, op1) = loadOpBinary(xi, irb, eOpConv::NOTHING);

	unsigned bits = 0;
	bool high = false;
	switch (i->id)
	{
	case X86_INS_PUNPCKLBW: bits = 8; break;
	case X86_INS_PUNPCKLWD: bits = 16; break;
	case X86_INS_PUNPCKLDQ: bits = 32; break;
	case X86_INS_PUNPCKLQDQ: bits = 64; break;
	case X86_INS_PUNPCKHBW:
		bits = 8;
		high = true;
		break;
	case X86_INS_PUNPCKHWD:
		bits = 16;
		high = true;
		break;
	case X86_INS_PUNPCKHDQ:
		bits = 32;
		high = true;
		break;
	case X86_INS_PUNPCKHQDQ:
		bits = 64;
		high = true;
		break;
	default: throw GenericError("translateSsePunpckl(): unhandled instruction id");
	}
	unsigned lanes = 128 / bits;
	auto* vecTy = FixedVectorType::get(irb.getIntNTy(bits), lanes);
	Value* v0 = irb.CreateBitCast(toI128(op0, irb), vecTy);
	Value* v1 = irb.CreateBitCast(toI128(op1, irb), vecTy);

	// Low:  [v0[0], v1[0], v0[1], v1[1], ...]
	// High: [v0[h], v1[h], v0[h+1], v1[h+1], ...] where h = lanes / 2
	unsigned halfLanes = lanes / 2;
	unsigned base = high ? halfLanes : 0;
	std::vector<int> mask;
	mask.reserve(lanes);
	for (unsigned n = 0; n < halfLanes; ++n)
	{
		mask.push_back(static_cast<int>(base + n));
		mask.push_back(static_cast<int>(lanes + base + n));
	}
	Value* result = irb.CreateShuffleVector(v0, v1, mask);
	storeOp(xi->operands[0], irb.CreateBitCast(result, irb.getInt128Ty()), irb, eOpConv::NOTHING);
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

    std::vector<int> mask = {
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

/**
 * ADDSUBPS — alternating add/sub: even lanes subtract, odd lanes add.
 * HADDPS — horizontal add: dst[0]=src1[0]+src1[1], dst[1]=src1[2]+src1[3],
 *           dst[2]=src2[0]+src2[1], dst[3]=src2[2]+src2[3].
 * HSUBPS — horizontal sub.
 *
 * The plain lane-wise forms this used to also carry -- ADDPS and ADDSS -- are
 * in translateSseFltArith with their SUB, MUL, DIV, PD and SD counterparts.
 */
void Capstone2LlvmIrTranslatorX86_impl::translateSseHorizontal(cs_insn* i, cs_x86* xi, IRBuilder<>& irb)
{
	EXPECT_IS_BINARY(i, xi, irb);
	std::tie(op0, op1) = loadOpBinary(xi, irb, eOpConv::NOTHING);

	if (i->id == X86_INS_ADDSUBPS)
	{
		// Even lanes: subtract; odd lanes: add.
		auto* vec4f = vecType(irb.getFloatTy(), 4);
		Value* v0 = irb.CreateBitCast(toI128(op0, irb), vec4f);
		Value* v1 = irb.CreateBitCast(toI128(op1, irb), vec4f);
		Value* adds = irb.CreateFAdd(v0, v1);
        Value* subs = irb.CreateFSub(v0, v1);
        // Blend: even from subs, odd from adds.
        std::vector<int> mask = {4, 1, 6, 3}; // subs[0], adds[1], subs[2], adds[3]
        Value* blended = irb.CreateShuffleVector(subs, adds, mask);
        storeOp(xi->operands[0], irb.CreateBitCast(blended, irb.getInt128Ty()),
                irb, eOpConv::NOTHING);
	}
	else if (i->id == X86_INS_HADDPS)
	{
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
	}
	else if (i->id == X86_INS_HSUBPS)
	{
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
 * CVTPS2DQ — packed float32 → packed int32, rounding to nearest-even.
 * CVTTPS2DQ — the same, truncating toward zero.
 *
 * The extra T is the only difference between them and it was not being read:
 * this rounded nothing and CVTTPS2DQ was not in the table at all.
 */
void Capstone2LlvmIrTranslatorX86_impl::translateCvtPs2Dq(
        cs_insn* i, cs_x86* xi, IRBuilder<>& irb) {
    EXPECT_IS_BINARY(i, xi, irb);
    auto* src = loadOpBinaryOp1(xi, irb);
    auto* vec4f = vecType(irb.getFloatTy(), 4);
    auto* vec4i = vecType(irb.getInt32Ty(), 4);
    Value* v   = irb.CreateBitCast(toI128(src, irb), vec4f);
	if (i->id != X86_INS_CVTTPS2DQ)
	{
		v = irb.CreateUnaryIntrinsic(Intrinsic::roundeven, v);
	}
	Value* res = irb.CreateFPToSI(v, vec4i);
	storeOp(xi->operands[0], irb.CreateBitCast(res, irb.getInt128Ty()), irb, eOpConv::NOTHING);
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
	case X86_INS_SQRTPS: out = {false, false}; return true;
	case X86_INS_ADDSS:
	case X86_INS_SUBSS:
	case X86_INS_MULSS:
	case X86_INS_DIVSS:
	case X86_INS_MAXSS:
	case X86_INS_MINSS:
	case X86_INS_SQRTSS: out = {false, true}; return true;
	case X86_INS_ADDPD:
	case X86_INS_SUBPD:
	case X86_INS_MULPD:
	case X86_INS_DIVPD:
	case X86_INS_MAXPD:
	case X86_INS_MINPD:
	case X86_INS_SQRTPD: out = {true, false}; return true;
	case X86_INS_ADDSD:
	case X86_INS_SUBSD:
	case X86_INS_MULSD:
	case X86_INS_DIVSD:
	case X86_INS_MAXSD:
	case X86_INS_MINSD:
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
	EXPECT_IS_BINARY(i, xi, irb);

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
	case X86_INS_ADDSD: opcode = Instruction::FAdd; break;
	case X86_INS_SUBPS:
	case X86_INS_SUBSS:
	case X86_INS_SUBPD:
	case X86_INS_SUBSD: opcode = Instruction::FSub; break;
	case X86_INS_MULPS:
	case X86_INS_MULSS:
	case X86_INS_MULPD:
	case X86_INS_MULSD: opcode = Instruction::FMul; break;
	case X86_INS_DIVPS:
	case X86_INS_DIVSS:
	case X86_INS_DIVPD:
	case X86_INS_DIVSD: opcode = Instruction::FDiv; break;
	default: translatePseudoAsmGeneric(i, xi, irb); return;
	}

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
	EXPECT_IS_BINARY(i, xi, irb);

	FltForm f;
	if (!fltForm(i->id, f))
	{
		translatePseudoAsmGeneric(i, xi, irb);
		return;
	}
	bool isMax = i->id == X86_INS_MAXPS || i->id == X86_INS_MAXSS || i->id == X86_INS_MAXPD || i->id == X86_INS_MAXSD;

	std::tie(op0, op1) = loadOpBinary(xi, irb, eOpConv::NOTHING);

	Type* elemTy = f.isDouble ? irb.getDoubleTy() : irb.getFloatTy();
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
	EXPECT_IS_BINARY(i, xi, irb);

	FltForm f;
	if (!fltForm(i->id, f))
	{
		translatePseudoAsmGeneric(i, xi, irb);
		return;
	}

	std::tie(op0, op1) = loadOpBinary(xi, irb, eOpConv::NOTHING);

	Type* elemTy = f.isDouble ? irb.getDoubleTy() : irb.getFloatTy();
	unsigned n = f.isDouble ? 2 : 4;
	auto* vecTy = FixedVectorType::get(elemTy, n);
	Value* vSrc = irb.CreateBitCast(toI128(op1, irb), vecTy);

	auto sqrtOf = [&](Value* v) { return irb.CreateUnaryIntrinsic(Intrinsic::sqrt, v); };

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

	bool isDouble = i->id == X86_INS_UCOMISD || i->id == X86_INS_COMISD;
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
	EXPECT_IS_BINARY(i, xi, irb);
	std::tie(op0, op1) = loadOpBinary(xi, irb, eOpConv::NOTHING);
	Value* a = toI128(op0, irb);
	Value* b = toI128(op1, irb);

	Value* res = nullptr;
	switch (i->id)
	{
	case X86_INS_ANDPS:
	case X86_INS_ANDPD: res = irb.CreateAnd(a, b); break;
	case X86_INS_ANDNPS:
	case X86_INS_ANDNPD: res = irb.CreateAnd(irb.CreateNot(a), b); break;
	case X86_INS_ORPS:
	case X86_INS_ORPD: res = irb.CreateOr(a, b); break;
	case X86_INS_XORPS:
	case X86_INS_XORPD: res = irb.CreateXor(a, b); break;
	default: translatePseudoAsmGeneric(i, xi, irb); return;
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
	EXPECT_IS_BINARY(i, xi, irb);

	if (i->id == X86_INS_MOVSD && !isXmmOp(xi->operands[0]) && !isXmmOp(xi->operands[1]))
	{
		translateMoveString(i, xi, irb);
		return;
	}

	unsigned bits = (i->id == X86_INS_MOVSD) ? 64 : 32;
	auto* laneTy = irb.getIntNTy(bits);

	// Destination is memory: write the low lane and nothing else.
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
		// Loading from memory zeroes the rest of the register.
		storeOp(xi->operands[0], lane, irb, eOpConv::NOTHING);
		return;
	}

	// Register to register: keep everything above the low lane.
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
	EXPECT_IS_BINARY(i, xi, irb);

	bool high = i->id == X86_INS_MOVHPS || i->id == X86_INS_MOVHPD;
	auto* i128 = irb.getInt128Ty();
	auto* i64t = irb.getInt64Ty();

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
	EXPECT_IS_BINARY(i, xi, irb);

	auto* i128 = irb.getInt128Ty();
	auto* vec4f = vecType(irb.getFloatTy(), 4);
	auto* vec2d = vecType(irb.getDoubleTy(), 2);
	auto* vec4i = vecType(irb.getInt32Ty(), 4);

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

	bool isDouble = i->id == X86_INS_CVTTSD2SI;
	auto* vecTy = isDouble ? vecType(irb.getDoubleTy(), 2) : vecType(irb.getFloatTy(), 4);
	Value* src = toI128(loadOp(xi->operands[1], irb), irb);
	Value* lane = irb.CreateExtractElement(irb.CreateBitCast(src, vecTy), (uint64_t)0);

	unsigned destBits = xi->operands[0].size ? xi->operands[0].size * 8 : 32;
	storeOp(xi->operands[0], irb.CreateFPToSI(lane, irb.getIntNTy(destBits)), irb, eOpConv::ZEXT_TRUNC_OR_BITCAST);
}

/**
 * UNPCKLPS/UNPCKHPS, UNPCKLPD/UNPCKHPD — interleave lanes from the two halves.
 */
void Capstone2LlvmIrTranslatorX86_impl::translateSseUnpck(cs_insn* i, cs_x86* xi, IRBuilder<>& irb)
{
	EXPECT_IS_BINARY(i, xi, irb);

	bool isDouble = i->id == X86_INS_UNPCKLPD || i->id == X86_INS_UNPCKHPD;
	bool high = i->id == X86_INS_UNPCKHPS || i->id == X86_INS_UNPCKHPD;
	Type* elemTy = isDouble ? irb.getDoubleTy() : irb.getFloatTy();
	unsigned n = isDouble ? 2 : 4;
	auto* vecTy = FixedVectorType::get(elemTy, n);

	std::tie(op0, op1) = loadOpBinary(xi, irb, eOpConv::NOTHING);
	Value* a = irb.CreateBitCast(toI128(op0, irb), vecTy);
	Value* b = irb.CreateBitCast(toI128(op1, irb), vecTy);

	std::vector<int> mask;
	unsigned base = high ? n / 2 : 0;
	for (unsigned k = 0; k < n / 2; ++k)
	{
		mask.push_back(static_cast<int>(base + k));
		mask.push_back(static_cast<int>(n + base + k));
	}
	storeOp(
		xi->operands[0],
		irb.CreateBitCast(irb.CreateShuffleVector(a, b, mask), irb.getInt128Ty()),
		irb,
		eOpConv::NOTHING);
}

/**
 * SHUFPS, SHUFPD — the low half of the result is picked out of the
 * destination, the high half out of the source, both by the immediate.
 */
void Capstone2LlvmIrTranslatorX86_impl::translateSseShufp(cs_insn* i, cs_x86* xi, IRBuilder<>& irb)
{
	EXPECT_IS_TERNARY(i, xi, irb);

	bool isDouble = i->id == X86_INS_SHUFPD;
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
void Capstone2LlvmIrTranslatorX86_impl::translateSseMovMsk(cs_insn* i, cs_x86* xi, IRBuilder<>& irb)
{
	EXPECT_IS_BINARY(i, xi, irb);

	bool isDouble = i->id == X86_INS_MOVMSKPD;
	bool isByte = i->id == X86_INS_PMOVMSKB;
	unsigned n = isByte ? 16 : (isDouble ? 2 : 4);
	unsigned laneBits = isByte ? 8 : (isDouble ? 64 : 32);
	auto* vecTy = FixedVectorType::get(irb.getIntNTy(laneBits), n);

	Value* v = irb.CreateBitCast(toI128(loadOp(xi->operands[1], irb), irb), vecTy);
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
void Capstone2LlvmIrTranslatorX86_impl::translateSsePalignr(cs_insn* i, cs_x86* xi, IRBuilder<>& irb)
{
	EXPECT_IS_TERNARY(i, xi, irb);

	Value* dst = toI128(loadOp(xi->operands[0], irb), irb);
	Value* src = toI128(loadOp(xi->operands[1], irb), irb);
	auto* i128 = irb.getInt128Ty();
	unsigned n = static_cast<unsigned>(xi->operands[2].imm) & 0xff;

	Value* res = nullptr;
	if (n == 0)
	{
		res = src;
	}
	else if (n < 16)
	{
		Value* lo = irb.CreateLShr(src, ConstantInt::get(i128, n * 8));
		Value* hi = irb.CreateShl(dst, ConstantInt::get(i128, 128 - n * 8));
		res = irb.CreateOr(hi, lo);
	}
	else if (n < 32)
	{
		res = irb.CreateLShr(dst, ConstantInt::get(i128, (n - 16) * 8));
	}
	else
	{
		res = ConstantInt::get(i128, 0);
	}
	storeOp(xi->operands[0], res, irb, eOpConv::NOTHING);
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
	EXPECT_IS_BINARY(i, xi, irb);
	std::tie(op0, op1) = loadOpBinary(xi, irb, eOpConv::NOTHING);

	unsigned bits = i->id == X86_INS_PAVGB ? 8 : 16;
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
	case X86_INS_VPCMPEQW:
	case X86_INS_VPCMPGTW: laneBits = 16; break;
	case X86_INS_VPADDD:
	case X86_INS_VPSUBD:
	case X86_INS_VPCMPEQD:
	case X86_INS_VPCMPGTD: laneBits = 32; break;
	case X86_INS_VPADDQ:
	case X86_INS_VPSUBQ:
	case X86_INS_VPCMPEQQ:
	case X86_INS_VPCMPGTQ: laneBits = 64; break;
	// The bitwise operations are lane-independent.
	case X86_INS_VPAND:
	case X86_INS_VPOR:
	case X86_INS_VPXOR:
	case X86_INS_VPANDN: laneBits = bits; break;
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
		case X86_INS_VPAND: res = irb.CreateAnd(a, b); break;
		case X86_INS_VPOR: res = irb.CreateOr(a, b); break;
		case X86_INS_VPXOR: res = irb.CreateXor(a, b); break;
		// ANDN is the FIRST operand inverted, not the second.
		case X86_INS_VPANDN: res = irb.CreateAnd(irb.CreateNot(a), b); break;
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

	bool isDouble = false;
	unsigned op = 0;
	switch (i->id)
	{
	case X86_INS_VADDPS: op = 0; break;
	case X86_INS_VSUBPS: op = 1; break;
	case X86_INS_VMULPS: op = 2; break;
	case X86_INS_VDIVPS: op = 3; break;
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
	default: translatePseudoAsmGeneric(i, xi, irb); return;
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

} // namespace capstone2llvmir
} // namespace retdec
