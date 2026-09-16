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

#include <capstone/capstone.h>
#include <llvm/IR/Constants.h>
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

} // namespace capstone2llvmir
} // namespace retdec
