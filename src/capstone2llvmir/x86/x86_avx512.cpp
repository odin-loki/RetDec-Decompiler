/**
 * @file src/capstone2llvmir/x86/x86_avx512.cpp
 * @brief AVX-512 opmask register instructions for the x86 capstone2llvmir lifter.
 * @copyright (c) 2025-2026 Odin Loch trading as Imortek, licensed under the MIT license
 *
 * The k0..k7 opmask registers already existed in the register file as i64
 * globals; what did not exist was a single instruction that reads or writes
 * them. Every KMOV/KAND/KOR/KXOR/KSHIFT/KUNPCK/KORTEST/KTEST/KADD id was
 * registered as `nullptr` in x86_init.cpp -- and KADD*, KTEST*, KUNPCKDQ and
 * KUNPCKWD were not registered at all -- so all of them fell through to
 * pseudo-assembly.
 *
 * Two things about these instructions are easy to get wrong and are the
 * reason the tests here use the inputs they do:
 *
 *   1. The width is in the mnemonic suffix, NOT in the operand. Capstone
 *      reports `size = 2` for every opmask operand, whatever the instruction:
 *      `kmovq k1, rax` and `kmovb k1, eax` both say 2. Reading the width off
 *      the operand would make KMOVB and KMOVQ the same instruction.
 *
 *   2. The destination's bits above the instruction's width are ZEROED, not
 *      preserved. `kmovw k1, k2` clears k1[63:16]. Since the register file
 *      holds k0..k7 as i64, a translator that merged into the old value --
 *      the usual sub-register rule elsewhere in x86 -- would be wrong for
 *      every one of these.
 */

#include <string>

#include <capstone/capstone.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/Type.h>
#include <llvm/IR/Value.h>

#include "capstone2llvmir/x86/x86_impl.h"

namespace retdec {
namespace capstone2llvmir {

static bool isMaskRegister(uint32_t r)
{
	return X86_REG_K0 <= r && r <= X86_REG_K7;
}

/**
 * The width an opmask instruction operates at, taken from the mnemonic
 * suffix. Capstone's operand `size` is 2 for every opmask operand regardless
 * of the instruction, so it cannot be used for this.
 *
 * Returns 0 for an id that is not an opmask instruction.
 */
unsigned Capstone2LlvmIrTranslatorX86_impl::maskWidth(cs_insn* i)
{
	switch (i->id)
	{
	case X86_INS_KADDB:
	case X86_INS_KANDB:
	case X86_INS_KANDNB:
	case X86_INS_KMOVB:
	case X86_INS_KNOTB:
	case X86_INS_KORB:
	case X86_INS_KORTESTB:
	case X86_INS_KSHIFTLB:
	case X86_INS_KSHIFTRB:
	case X86_INS_KTESTB:
	case X86_INS_KXNORB:
	case X86_INS_KXORB: return 8;
	case X86_INS_KADDW:
	case X86_INS_KANDW:
	case X86_INS_KANDNW:
	case X86_INS_KMOVW:
	case X86_INS_KNOTW:
	case X86_INS_KORW:
	case X86_INS_KORTESTW:
	case X86_INS_KSHIFTLW:
	case X86_INS_KSHIFTRW:
	case X86_INS_KTESTW:
	case X86_INS_KUNPCKBW:
	case X86_INS_KXNORW:
	case X86_INS_KXORW: return 16;
	case X86_INS_KADDD:
	case X86_INS_KANDD:
	case X86_INS_KANDND:
	case X86_INS_KMOVD:
	case X86_INS_KNOTD:
	case X86_INS_KORD:
	case X86_INS_KORTESTD:
	case X86_INS_KSHIFTLD:
	case X86_INS_KSHIFTRD:
	case X86_INS_KTESTD:
	case X86_INS_KUNPCKWD:
	case X86_INS_KXNORD:
	case X86_INS_KXORD: return 32;
	case X86_INS_KADDQ:
	case X86_INS_KANDQ:
	case X86_INS_KANDNQ:
	case X86_INS_KMOVQ:
	case X86_INS_KNOTQ:
	case X86_INS_KORQ:
	case X86_INS_KORTESTQ:
	case X86_INS_KSHIFTLQ:
	case X86_INS_KSHIFTRQ:
	case X86_INS_KTESTQ:
	case X86_INS_KUNPCKDQ:
	case X86_INS_KXNORQ:
	case X86_INS_KXORQ: return 64;
	default: return 0;
	}
}

/**
 * An operand read as an opmask value of exactly @p bits bits.
 *
 * An opmask register is held as i64, so this truncates; a general-purpose
 * register or memory operand is read at its own width and then narrowed.
 */
llvm::Value* Capstone2LlvmIrTranslatorX86_impl::loadMaskOp(cs_x86_op& op, unsigned bits, llvm::IRBuilder<>& irb)
{
	auto* ty = irb.getIntNTy(bits);

	if (op.type == X86_OP_REG && isMaskRegister(op.reg))
	{
		return irb.CreateZExtOrTrunc(loadRegister(op.reg, irb), ty);
	}

	return loadOp(op, irb, ty, false);
}

/**
 * Write an opmask result back.
 *
 * Into an opmask register this ZERO-extends: every one of these instructions
 * clears the destination bits above its own width. Into a general-purpose
 * register it zero-extends to that register's width, which is what the
 * KMOV reg, k form specifies. Into memory it stores exactly @p bits bits.
 */
void Capstone2LlvmIrTranslatorX86_impl::storeMaskOp(
	cs_x86_op& op, llvm::Value* val, unsigned bits, llvm::IRBuilder<>& irb)
{
	val = irb.CreateZExtOrTrunc(val, irb.getIntNTy(bits));

	if (op.type == X86_OP_REG && isMaskRegister(op.reg))
	{
		storeRegister(op.reg, irb.CreateZExt(val, irb.getInt64Ty()), irb, eOpConv::NOTHING);
		return;
	}

	storeOp(op, val, irb, eOpConv::ZEXT_TRUNC_OR_BITCAST);
}

/**
 * The flags KORTEST and KTEST share: everything except ZF and CF is cleared.
 */
void Capstone2LlvmIrTranslatorX86_impl::storeMaskTestFlags(llvm::Value* zf, llvm::Value* cf, llvm::IRBuilder<>& irb)
{
	auto* f = irb.getFalse();
	storeRegisters(
		irb, {{X86_REG_ZF, zf}, {X86_REG_CF, cf}, {X86_REG_OF, f}, {X86_REG_SF, f}, {X86_REG_AF, f}, {X86_REG_PF, f}});
}

/**
 * KMOVB/KMOVW/KMOVD/KMOVQ -- between opmask registers, general-purpose
 * registers and memory.
 */
void Capstone2LlvmIrTranslatorX86_impl::translateKmov(cs_insn* i, cs_x86* xi, llvm::IRBuilder<>& irb)
{
	EXPECT_IS_BINARY(i, xi, irb);

	unsigned bits = maskWidth(i);
	llvm::Value* src = loadMaskOp(xi->operands[1], bits, irb);
	storeMaskOp(xi->operands[0], src, bits, irb);
}

/**
 * The three-operand bitwise and arithmetic opmask instructions.
 *
 * Intel writes these as DEST, SRC1, SRC2 where SRC1 is the VEX.vvvv operand
 * -- the SECOND one listed -- and SRC2 is the r/m operand, the third. That
 * ordering only shows up in KANDN (`~SRC1 & SRC2`), which is why the test
 * for it uses two different values.
 */
void Capstone2LlvmIrTranslatorX86_impl::translateKopBinary(cs_insn* i, cs_x86* xi, llvm::IRBuilder<>& irb)
{
	EXPECT_IS_TERNARY(i, xi, irb);

	unsigned bits = maskWidth(i);
	llvm::Value* s1 = loadMaskOp(xi->operands[1], bits, irb);
	llvm::Value* s2 = loadMaskOp(xi->operands[2], bits, irb);
	llvm::Value* res = nullptr;

	switch (i->id)
	{
	case X86_INS_KADDB:
	case X86_INS_KADDW:
	case X86_INS_KADDD:
	case X86_INS_KADDQ: res = irb.CreateAdd(s1, s2); break;
	case X86_INS_KANDB:
	case X86_INS_KANDW:
	case X86_INS_KANDD:
	case X86_INS_KANDQ: res = irb.CreateAnd(s1, s2); break;
	case X86_INS_KANDNB:
	case X86_INS_KANDNW:
	case X86_INS_KANDND:
	case X86_INS_KANDNQ: res = irb.CreateAnd(irb.CreateNot(s1), s2); break;
	case X86_INS_KORB:
	case X86_INS_KORW:
	case X86_INS_KORD:
	case X86_INS_KORQ: res = irb.CreateOr(s1, s2); break;
	case X86_INS_KXORB:
	case X86_INS_KXORW:
	case X86_INS_KXORD:
	case X86_INS_KXORQ: res = irb.CreateXor(s1, s2); break;
	case X86_INS_KXNORB:
	case X86_INS_KXNORW:
	case X86_INS_KXNORD:
	case X86_INS_KXNORQ: res = irb.CreateNot(irb.CreateXor(s1, s2)); break;
	default: translatePseudoAsmGeneric(i, xi, irb); return;
	}

	storeMaskOp(xi->operands[0], res, bits, irb);
}

/**
 * KNOTB/W/D/Q.
 */
void Capstone2LlvmIrTranslatorX86_impl::translateKnot(cs_insn* i, cs_x86* xi, llvm::IRBuilder<>& irb)
{
	EXPECT_IS_BINARY(i, xi, irb);

	unsigned bits = maskWidth(i);
	storeMaskOp(xi->operands[0], irb.CreateNot(loadMaskOp(xi->operands[1], bits, irb)), bits, irb);
}

/**
 * KSHIFTL/KSHIFTR.
 *
 * The count is the full imm8 and is NOT taken modulo the width: a count of
 * 16 or more on a 16-bit mask gives zero, where an LLVM `shl` by 16 of an
 * i16 would be poison. The count is a literal, so the choice is made here
 * rather than with a select.
 */
void Capstone2LlvmIrTranslatorX86_impl::translateKshift(cs_insn* i, cs_x86* xi, llvm::IRBuilder<>& irb)
{
	EXPECT_IS_TERNARY(i, xi, irb);

	unsigned bits = maskWidth(i);
	auto* ty = irb.getIntNTy(bits);

	if (xi->operands[2].type != X86_OP_IMM)
	{
		translatePseudoAsmGeneric(i, xi, irb);
		return;
	}

	uint64_t cnt = static_cast<uint64_t>(xi->operands[2].imm) & 0xff;
	llvm::Value* src = loadMaskOp(xi->operands[1], bits, irb);
	llvm::Value* res = nullptr;

	if (cnt >= bits)
	{
		res = llvm::ConstantInt::get(ty, 0);
	}
	else
	{
		bool left = i->id == X86_INS_KSHIFTLB || i->id == X86_INS_KSHIFTLW || i->id == X86_INS_KSHIFTLD
				 || i->id == X86_INS_KSHIFTLQ;
		auto* amount = llvm::ConstantInt::get(ty, cnt);
		res = left ? irb.CreateShl(src, amount) : irb.CreateLShr(src, amount);
	}

	storeMaskOp(xi->operands[0], res, bits, irb);
}

/**
 * KUNPCKBW/KUNPCKWD/KUNPCKDQ -- concatenate two half-width masks.
 *
 * The result is twice as wide as the sources, and the SRC1 operand -- the
 * second one listed -- becomes the HIGH half. Getting that backwards is
 * invisible unless the two sources differ, which is why the test uses
 * 0xaa and 0x55.
 */
void Capstone2LlvmIrTranslatorX86_impl::translateKunpck(cs_insn* i, cs_x86* xi, llvm::IRBuilder<>& irb)
{
	EXPECT_IS_TERNARY(i, xi, irb);

	unsigned bits = maskWidth(i);
	unsigned half = bits / 2;
	auto* ty = irb.getIntNTy(bits);

	llvm::Value* hi = irb.CreateZExt(loadMaskOp(xi->operands[1], half, irb), ty);
	llvm::Value* lo = irb.CreateZExt(loadMaskOp(xi->operands[2], half, irb), ty);

	llvm::Value* res = irb.CreateOr(irb.CreateShl(hi, llvm::ConstantInt::get(ty, half)), lo);
	storeMaskOp(xi->operands[0], res, bits, irb);
}

/**
 * KORTESTB/W/D/Q.
 *
 * ZF is set when the OR is zero and CF when it is all ones AT THE
 * INSTRUCTION'S WIDTH -- 0xff for KORTESTB, not 0xffff. Everything else is
 * cleared.
 */
void Capstone2LlvmIrTranslatorX86_impl::translateKortest(cs_insn* i, cs_x86* xi, llvm::IRBuilder<>& irb)
{
	EXPECT_IS_BINARY(i, xi, irb);

	unsigned bits = maskWidth(i);
	auto* ty = irb.getIntNTy(bits);

	llvm::Value* tmp = irb.CreateOr(loadMaskOp(xi->operands[0], bits, irb), loadMaskOp(xi->operands[1], bits, irb));

	llvm::Value* zf = irb.CreateICmpEQ(tmp, llvm::ConstantInt::get(ty, 0));
	llvm::Value* cf = irb.CreateICmpEQ(tmp, llvm::ConstantInt::getAllOnesValue(ty));
	storeMaskTestFlags(zf, cf, irb);
}

/**
 * KTESTB/W/D/Q.
 *
 * ZF from `SRC2 & SRC1`, CF from `SRC2 & ~SRC1`. The two are different
 * expressions, so a test whose operands make them agree proves nothing.
 */
void Capstone2LlvmIrTranslatorX86_impl::translateKtest(cs_insn* i, cs_x86* xi, llvm::IRBuilder<>& irb)
{
	EXPECT_IS_BINARY(i, xi, irb);

	unsigned bits = maskWidth(i);
	auto* ty = irb.getIntNTy(bits);
	auto* zero = llvm::ConstantInt::get(ty, 0);

	llvm::Value* s1 = loadMaskOp(xi->operands[0], bits, irb);
	llvm::Value* s2 = loadMaskOp(xi->operands[1], bits, irb);

	llvm::Value* zf = irb.CreateICmpEQ(irb.CreateAnd(s2, s1), zero);
	llvm::Value* cf = irb.CreateICmpEQ(irb.CreateAnd(s2, irb.CreateNot(s1)), zero);
	storeMaskTestFlags(zf, cf, irb);
}

//
//==============================================================================
// Comparisons whose destination is an opmask register.
//==============================================================================
//
// These cannot be dispatched on the capstone instruction id, because for the
// EVEX VPCMP family the id is wrong. Capstone 5.0.9 returns
// `X86_INS_VPCMPB + predicate`, ignoring both the element width and the
// signedness -- so a single id covers up to nine different instructions:
//
//   id 1113 (X86_INS_VPCMPEQB)    vpcmpeqb vpcmpleb vpcmpled vpcmpleq
//                                 vpcmpleub vpcmpleud vpcmpleuq vpcmpleuw
//                                 vpcmplew
//   id 1116 (X86_INS_VPCMPEQW)    vpcmpeqw and every vpcmpnlt* form
//   id 1117 (X86_INS_VPCMPESTRI)  every vpcmpnle* form -- an SSE4.2 string
//                                 instruction's id, handed to a 512-bit
//                                 vector compare
//
// The mnemonic string is right in every case, so that is what these read.
// Anything that does not parse as `vpcmp<pred>[u]<size>` falls back, which is
// what keeps a genuine `vpcmpestri` out of here.
//

namespace {

/// The predicate encoding shared by VPCMP and VPCMPU.
enum : unsigned
{
	CMP_EQ = 0,
	CMP_LT = 1,
	CMP_LE = 2,
	CMP_FALSE = 3,
	CMP_NEQ = 4,
	CMP_NLT = 5,
	CMP_NLE = 6,
	CMP_TRUE = 7,
};

struct VectorCompareForm
{
	unsigned laneBits = 0;
	unsigned pred = 0;
	bool isSigned = true;
	bool fromImmediate = false;
	bool ok = false;
};

/**
 * Read `vpcmp<pred>[u]<size>` out of a capstone mnemonic.
 *
 * Parsing runs back to front: the last character is the element size, a `u`
 * before it makes the comparison unsigned, and what remains is the predicate.
 * No predicate name ends in `u`, so that split is unambiguous.
 *
 * An empty predicate is the base form -- `vpcmpb`, `vpcmpud` -- which capstone
 * emits for predicates 3 and 7 and which carries the predicate in a fourth
 * operand instead.
 */
VectorCompareForm parseVectorCompare(const char* m)
{
	VectorCompareForm f;
	std::string s(m ? m : "");
	if (s.compare(0, 5, "vpcmp") != 0 || s.size() < 6)
	{
		return f;
	}
	s = s.substr(5);

	switch (s.back())
	{
	case 'b': f.laneBits = 8; break;
	case 'w': f.laneBits = 16; break;
	case 'd': f.laneBits = 32; break;
	case 'q': f.laneBits = 64; break;
	default: return f;
	}
	s.pop_back();

	if (!s.empty() && s.back() == 'u')
	{
		f.isSigned = false;
		s.pop_back();
	}

	if (s.empty())
	{
		f.fromImmediate = true;
	}
	else if (s == "eq")
	{
		f.pred = CMP_EQ;
	}
	else if (s == "lt")
	{
		f.pred = CMP_LT;
	}
	else if (s == "le")
	{
		f.pred = CMP_LE;
	}
	else if (s == "neq")
	{
		f.pred = CMP_NEQ;
	}
	else if (s == "nlt")
	{
		f.pred = CMP_NLT;
	}
	else if (s == "nle")
	{
		f.pred = CMP_NLE;
	}
	else if (s == "gt")
	{
		// VPCMPGT is signed-only and has no immediate form.
		f.pred = CMP_NLE;
		f.isSigned = true;
	}
	else
	{
		return f;
	}

	f.ok = true;
	return f;
}

} // anonymous namespace

/**
 * The width a mask-producing comparison operates at, taken from its source
 * operands -- the destination is an opmask register and says nothing.
 */
unsigned Capstone2LlvmIrTranslatorX86_impl::maskCompareWidth(cs_x86* xi)
{
	unsigned bits = 0;
	for (unsigned k = 1; k < xi->op_count; ++k)
	{
		auto& op = xi->operands[k];
		if (op.type != X86_OP_REG)
		{
			continue;
		}
		unsigned b = vectorRegisterWidth(op.reg);
		if (b == 0 || (bits != 0 && b != bits))
		{
			return 0;
		}
		bits = b;
	}

	if (bits == 0)
	{
		return 0;
	}

	for (unsigned k = 1; k < xi->op_count; ++k)
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
 * Write a lane-wise predicate into an opmask register.
 *
 * One bit per lane, bit 0 for lane 0, and the bits above the lane count are
 * cleared -- the same rule every opmask write follows.
 */
void Capstone2LlvmIrTranslatorX86_impl::storeLaneMask(cs_x86_op& dst, llvm::Value* lanes, llvm::IRBuilder<>& irb)
{
	auto* vecTy = llvm::cast<llvm::FixedVectorType>(lanes->getType());
	unsigned n = vecTy->getNumElements();
	storeMaskOp(dst, irb.CreateBitCast(lanes, irb.getIntNTy(n)), n, irb);
}

/**
 * VPCMPEQ/VPCMPGT/VPCMP/VPCMPU with an opmask destination, at 128, 256 or
 * 512 bits.
 *
 * With a VECTOR destination the same mnemonics are the AVX2 comparisons that
 * produce all-ones or all-zero lanes, which translateAvxPackedBinary already
 * handles -- so this routes there rather than answering a mask.
 */
void Capstone2LlvmIrTranslatorX86_impl::translateVectorCompare(cs_insn* i, cs_x86* xi, llvm::IRBuilder<>& irb)
{
	if (xi->op_count < 3 || !isMaskRegister(xi->operands[0].type == X86_OP_REG ? xi->operands[0].reg : X86_REG_INVALID))
	{
		translateAvxPackedBinary(i, xi, irb);
		return;
	}

	auto f = parseVectorCompare(i->mnemonic);
	if (!f.ok || hasEvexModifier(xi, 1))
	{
		translatePseudoAsmGeneric(i, xi, irb);
		return;
	}

	if (f.fromImmediate)
	{
		if (xi->op_count != 4 || xi->operands[3].type != X86_OP_IMM)
		{
			translatePseudoAsmGeneric(i, xi, irb);
			return;
		}
		f.pred = static_cast<unsigned>(xi->operands[3].imm) & 0x7;
	}
	else if (xi->op_count != 3)
	{
		translatePseudoAsmGeneric(i, xi, irb);
		return;
	}

	unsigned bits = maskCompareWidth(xi);
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

	unsigned n = bits / f.laneBits;
	auto* vecTy = llvm::FixedVectorType::get(irb.getIntNTy(f.laneBits), n);
	llvm::Value* va = irb.CreateBitCast(a, vecTy);
	llvm::Value* vb = irb.CreateBitCast(b, vecTy);

	llvm::Value* lanes = nullptr;
	switch (f.pred)
	{
	case CMP_EQ: lanes = irb.CreateICmpEQ(va, vb); break;
	case CMP_NEQ: lanes = irb.CreateICmpNE(va, vb); break;
	case CMP_LT: lanes = f.isSigned ? irb.CreateICmpSLT(va, vb) : irb.CreateICmpULT(va, vb); break;
	case CMP_LE: lanes = f.isSigned ? irb.CreateICmpSLE(va, vb) : irb.CreateICmpULE(va, vb); break;
	case CMP_NLT: lanes = f.isSigned ? irb.CreateICmpSGE(va, vb) : irb.CreateICmpUGE(va, vb); break;
	case CMP_NLE: lanes = f.isSigned ? irb.CreateICmpSGT(va, vb) : irb.CreateICmpUGT(va, vb); break;
	case CMP_FALSE: lanes = llvm::Constant::getNullValue(llvm::FixedVectorType::get(irb.getInt1Ty(), n)); break;
	default: lanes = llvm::Constant::getAllOnesValue(llvm::FixedVectorType::get(irb.getInt1Ty(), n)); break;
	}

	storeLaneMask(xi->operands[0], lanes, irb);
}

/**
 * VPTESTM and VPTESTNM.
 *
 * `vptestmb k1, zmm2, zmm1` sets k1[i] when the bitwise AND of lane i is
 * non-zero; VPTESTNM sets it when the AND is zero. These ids are the one part
 * of the family capstone gets right, so they dispatch normally.
 */
void Capstone2LlvmIrTranslatorX86_impl::translateVectorTestMask(cs_insn* i, cs_x86* xi, llvm::IRBuilder<>& irb)
{
	if (xi->op_count != 3 || xi->operands[0].type != X86_OP_REG || !isMaskRegister(xi->operands[0].reg)
		|| hasEvexModifier(xi, 1))
	{
		translatePseudoAsmGeneric(i, xi, irb);
		return;
	}

	unsigned laneBits = 0;
	bool negated = false;
	switch (i->id)
	{
	case X86_INS_VPTESTMB: laneBits = 8; break;
	case X86_INS_VPTESTMW: laneBits = 16; break;
	case X86_INS_VPTESTMD: laneBits = 32; break;
	case X86_INS_VPTESTMQ: laneBits = 64; break;
	case X86_INS_VPTESTNMB:
		laneBits = 8;
		negated = true;
		break;
	case X86_INS_VPTESTNMW:
		laneBits = 16;
		negated = true;
		break;
	case X86_INS_VPTESTNMD:
		laneBits = 32;
		negated = true;
		break;
	case X86_INS_VPTESTNMQ:
		laneBits = 64;
		negated = true;
		break;
	default: translatePseudoAsmGeneric(i, xi, irb); return;
	}

	unsigned bits = maskCompareWidth(xi);
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

	unsigned n = bits / laneBits;
	auto* vecTy = llvm::FixedVectorType::get(irb.getIntNTy(laneBits), n);
	llvm::Value* andv = irb.CreateAnd(irb.CreateBitCast(a, vecTy), irb.CreateBitCast(b, vecTy));
	auto* zero = llvm::Constant::getNullValue(vecTy);

	llvm::Value* lanes = negated ? irb.CreateICmpEQ(andv, zero) : irb.CreateICmpNE(andv, zero);
	storeLaneMask(xi->operands[0], lanes, irb);
}

} // namespace capstone2llvmir
} // namespace retdec
