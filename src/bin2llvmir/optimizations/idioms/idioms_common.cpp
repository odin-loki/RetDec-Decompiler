#include "retdec/bin2llvmir/optimizations/idioms/idioms_ext.h"
/**
 * @file src/bin2llvmir/optimizations/idioms/idioms_common.cpp
 * @brief Common compiler instruction idioms
 * @copyright (c) 2017 Avast Software, licensed under the MIT license
 * @copyright (c) 2025-2026 Odin Loch trading as Imortek (modifications)
 */

#include <cmath>

#include <llvm/IR/PatternMatch.h>

#include "retdec/bin2llvmir/optimizations/idioms/idioms_common.h"

using namespace llvm;
using namespace PatternMatch;

namespace retdec {
namespace bin2llvmir {

/**
 * Exchange shift left with a division
 *
 * @param iter value to visit
 * @return replaced Instruction, otherwise nullptr
 */
Instruction * IdiomsCommon::exchangeBitShiftUDiv(BasicBlock::iterator iter) const {
	Instruction & val = (*iter);
	Value * op0 = nullptr;
	ConstantInt * cnst = nullptr;

	// X u>> C --> X / 2^C
	if (match(&val, m_LShr(m_Value(op0), m_ConstantInt(cnst))) &&
			isPowerOfTwoRepresentable(cnst)) {
		Constant *NewCst = ConstantInt::get(cnst->getType(),
						pow(2, *cnst->getValue().getRawData()));
		BinaryOperator *div = BinaryOperator::CreateUDiv(op0, NewCst);
		return div;
	}

	return nullptr;
}

/**
 * Exchange x u>> 31 with x < 0
 *
 * @param iter value to visit
 * @return replaced Instruction, otherwise nullptr
 */
Instruction * IdiomsCommon::exchangeLessThanZero(BasicBlock::iterator iter) const {
	Instruction & val = (*iter);
	Value * op0 = nullptr;
	ConstantInt * cnst = nullptr;

	// X u>> 31 --> X < 0
	if (match(&val, m_LShr(m_Value(op0), m_ConstantInt(cnst)))
				&& *cnst->getValue().getRawData() == 31) {
		Constant *NewCst = ConstantInt::get(op0->getType(), 0);

		Instruction * cmp = CmpInst::Create(Instruction::ICmp, ICmpInst::ICMP_SLT, op0, NewCst);
		cmp->insertBefore(iter);

		return CastInst::CreateZExtOrBitCast(cmp, val.getType());
	}

	return nullptr;
}

/**
 * Exchange:
 *   ((X u>> 31) ^ 1)  --> X >= 0
 *   ((X ^ -1) u>> 31) --> X >= 0
 *
 * @param iter value to visit
 * @return replaced Instruction, otherwise nullptr
 */
Instruction * IdiomsCommon::exchangeGreaterEqualZero(BasicBlock::iterator iter) const {
	Instruction & val = (*iter);
	Value * op0 = nullptr;
	Value * op1 = nullptr;
	ConstantInt * cnst = nullptr;

	// ((X u>> 31) ^ 1) --> X >= 0
	//
	if ((match(&val, m_Xor(m_Value(op0), m_ConstantInt(cnst)))
			|| match(&val, m_Xor(m_ConstantInt(cnst), m_Value(op0))))
				&& cnst->isOne()) {

		if (match(op0, m_LShr(m_Value(op1), m_ConstantInt(cnst)))
				&& *cnst->getValue().getRawData() == 31) {

			Constant *NewCst = ConstantInt::get(op0->getType(), 0);

			Instruction * cmp = CmpInst::Create(Instruction::ICmp, ICmpInst::ICMP_SGE, op1, NewCst);
			cmp->insertBefore(iter);

			// erase lshr
			eraseInstFromBasicBlock(op0, val.getParent());

			return CastInst::CreateZExtOrBitCast(cmp, val.getType());
		}
	}
	// ((X ^ -1) u>> 31) --> X >= 0
	//
	else if (match(&val, m_LShr(m_Value(op0), m_ConstantInt(cnst)))
				&& *cnst->getValue().getRawData() == 31
				&& (match(op0, m_Xor(m_Value(op1), m_ConstantInt(cnst)))
					|| match(op0, m_Xor(m_ConstantInt(cnst), m_Value(op1))))
				&& cnst->isMinusOne()) {

		Constant *NewCst = ConstantInt::get(op1->getType(), 0);

		Instruction * cmp = CmpInst::Create(Instruction::ICmp, ICmpInst::ICMP_SGE, op1, NewCst);
		cmp->insertBefore(iter);

		return CastInst::CreateZExtOrBitCast(cmp, val.getType());
	}

	return nullptr;
}

/**
 * Exchange shift right with a division.
 * ((X >> 31) & mask) | (X >> shift)
 *
 * @param iter value to visit
 * @return replaced Instruction, otherwise nullptr
 */
Instruction * IdiomsCommon::exchangeBitShiftSDiv1(BasicBlock::iterator iter) const {
	Instruction & val = (*iter);
	Value * op_var1 = nullptr;
	Value * op_var2 = nullptr;
	Value * op_or = nullptr;
	Value * op_and = nullptr;
	Value * op_lshr = nullptr;
	Value * op_ashr = nullptr;
	ConstantInt* maskCnst = nullptr;
	ConstantInt* ashrCnst = nullptr;
	ConstantInt* shiftCnst = nullptr;

	// ((X s>> (w-1)) & mask) | (X u>> k)
	//
	// The sign bits are spread over the top k positions and the logical shift
	// supplies the rest, so the two halves reassemble exactly `ashr X, k`.
	// Measured over k = 1..3 and x in {-5, -4, -1, 0, 5, INT_MIN}: the
	// reassembly equals the arithmetic shift on every input.
	//
	// It was rewritten as `sdiv X, 2^k`, which is a different function. An
	// arithmetic shift floors; a signed division truncates toward zero. On the
	// same inputs they disagree wherever the dividend is negative and not an
	// exact multiple: for x = -5 and k = 1 the shift is -3 and the division is
	// -2. So the replacement is the shift.
	//
	// The matcher had its own problem: `op_var2` was bound from the lshr and
	// never compared with `op_var1` from the ashr, so
	// `((a s>> 31) & mask) | (b u>> 2)` was rewritten using `a` alone and `b`
	// was dropped. For a = 0, b = 16 the original is 4 and the rewrite was 0.
	// m_Or takes two m_Value and already matches either order, so the second
	// hand-written spelling is gone with it.
	if (!match(&val, m_Or(m_Value(op_and), m_Value(op_lshr)))) return nullptr;

	// The OR's two arms are not ordered, so try the other assignment too.
	for (int attempt = 0; attempt < 2; ++attempt)
	{
		if (attempt == 1)
		{
			std::swap(op_and, op_lshr);
		}

		if (!match(op_and, m_c_And(m_Value(op_ashr), m_ConstantInt(maskCnst)))) continue;
		if (!match(op_lshr, m_LShr(m_Value(op_var2), m_ConstantInt(shiftCnst)))) continue;

		const unsigned width = val.getType()->getIntegerBitWidth();

		if (!match(op_ashr, m_AShr(m_Value(op_var1), m_ConstantInt(ashrCnst))) || ashrCnst->getValue() != width - 1)
			continue;

		// The same X on both sides, or this is not one value being reassembled.
		if (op_var1 != op_var2) continue;

		if (!isPowerOfTwoRepresentable(shiftCnst)) continue;

		const unsigned shift = shiftCnst->getValue().getZExtValue();

		// The mask has to be exactly the positions the logical shift vacated.
		if (maskCnst->getValue() != ~llvm::APInt::getLowBitsSet(width, width - shift)) continue;

		Instruction* res = BinaryOperator::CreateAShr(op_var1, shiftCnst);

		eraseInstFromBasicBlock(op_ashr, val.getParent());
		eraseInstFromBasicBlock(op_and, val.getParent());
		eraseInstFromBasicBlock(op_lshr, val.getParent());
		eraseInstFromBasicBlock(op_or, val.getParent());

		return res;
	}

	return nullptr;
}

/**
 * Exchange shift left by with a multiplication
 *
 * @param iter value to visit
 * @return replaced Instruction, otherwise nullptr
 */
Instruction * IdiomsCommon::exchangeBitShiftMul(BasicBlock::iterator iter) const {
	Instruction & val = (*iter);
	Value * op0 = nullptr;
	ConstantInt * cnst = nullptr;

	// X << C --> X * 2^C
	if (match(&val, m_Shl(m_Value(op0), m_ConstantInt(cnst))) &&
			isPowerOfTwoRepresentable(cnst)) {
		Constant *NewCst = ConstantInt::get(cnst->getType(),
						pow(2, *cnst->getValue().getRawData()));
		BinaryOperator *mul = BinaryOperator::CreateMul(op0, NewCst);
		return mul;
	}

	return nullptr;
}

/**
 * Exchange x & (k - 1) with x % k
 *
 * @param iter value to visit
 * @return replaced Instruction, otherwise nullptr
 */
Instruction * IdiomsCommon::exchangeUnsignedModulo2n(BasicBlock::iterator iter) const {
	Instruction & val = (*iter);
	Value * op0 = nullptr;
	ConstantInt *cnst = nullptr;

	// X & (k - 1) --> X % k iff k is power of 2
	//
	// The "+ 1" has to happen at the constant's own width, and so does the
	// power-of-two test. isPowerOfTwo takes an `unsigned`, so for an i8 mask of
	// 0xFF it was asked about 0x100 -- a power of two -- while the modulus
	// actually built was `APInt(8, 0xFF) + 1`, which is 0. That is `urem i8 x,
	// 0`: undefined behaviour, from nothing more exotic than `and al, 0FFh`.
	if (match(&val, m_And(m_Value(op0), m_ConstantInt(cnst))))
	{
		llvm::APInt k = cnst->getValue() + 1;
		if (k.isPowerOf2())
		{
			Constant* NewCst = ConstantInt::get(op0->getType(), k);
			return BinaryOperator::CreateURem(op0, NewCst);
		}
	}

	return nullptr;
}

/**
 * Exchange -(((lshr(x, 31) + x) >> 1)) with -(x / 2)
 *
 * @param iter value to visit
 * @return replaced Instruction, otherwise nullptr
 */
Instruction * IdiomsCommon::exchangeDivByMinusTwo(BasicBlock::iterator iter) const {
	Instruction & val = (*iter);
	Value * op_add = nullptr;
	Value * op_ashr = nullptr;
	Value * op_add_op1 = nullptr;
	Value * op_add_op2 = nullptr;
	Value * op_x = nullptr;
	ConstantInt * cnst = nullptr;

	// 0 - (((X u>> 31) + X) s>> 1)) -- x / -2
	if (match(&val, m_Sub(m_ConstantInt(cnst), m_Value(op_ashr)))
			&& *cnst->getValue().getRawData() == 0) {

		if (match(op_ashr, m_AShr(m_Value(op_add), m_ConstantInt(cnst)))
				&& *cnst->getValue().getRawData() == 1) {

			if (match(op_add, m_Add(m_Value(op_add_op1), m_Value(op_add_op2)))) {

				if (match(op_add_op1, m_LShr(m_Value(op_x), m_ConstantInt(cnst)))
					|| match(op_add_op2, m_LShr(m_Value(op_x), m_ConstantInt(cnst)))) {
					/*
					 * Previous add is a commutative operation, there can be:
					 *  lshr + X
					 *    or:
					 *  X + lshr
					 * X from lshr can be used, because we know that it is a first use
					 */
					if (op_x == op_add_op1 || op_x == op_add_op2) {
						Constant *NewCst = ConstantInt::get(op_x->getType(), -2);
						Instruction * ret = BinaryOperator::CreateSDiv(op_x, NewCst);

						eraseInstFromBasicBlock(op_add, val.getParent());
						eraseInstFromBasicBlock(op_ashr, val.getParent());

						// which one is lshr? erase it!
						if (op_x == op_add_op1)
							eraseInstFromBasicBlock(op_add_op2, val.getParent());
						else
							eraseInstFromBasicBlock(op_add_op1, val.getParent());

						return ret;
					}
				}
			}
		}
	}

	return nullptr;
}

/*
 * Exchange (((lshr(lshr(X, 31), 27) + X) & N) - lshr((X >> 31), K)) with X % (N + 1)
 *
 * @param iter value to visit
 * @return replaced Instruction, otherwise nullptr
 */
Instruction * IdiomsCommon::exchangeSignedModulo2n(BasicBlock::iterator iter) const {
	Instruction & val = (*iter);
	Value * op_add = nullptr;
	Value * op_and = nullptr;
	Value * op_ashr = nullptr;
	Value * op_ashr1 = nullptr;
	Value * op_lshr = nullptr;
	Value * op_lshr1 = nullptr;
	Value * op_x = nullptr;
	Value * op_x_tmp = nullptr;
	ConstantInt * op_n = nullptr;
	ConstantInt * cnst = nullptr;

	// (((lshr(lshr(X, 31), 27) + X) & N) - lshr((X s>> 31), K)) --> X % (N + 1)
	if (! match(&val, m_Sub(m_Value(op_and), m_Value(op_lshr))))
		return nullptr;

	if (! match(op_lshr, m_LShr(m_Value(op_ashr), m_ConstantInt(cnst))))
		return nullptr;

	if (! match(op_ashr, m_AShr(m_Value(op_x), m_ConstantInt(cnst)))
			|| *cnst->getValue().getRawData() != 31) {
		return nullptr;
	}

	// left hand side of sub
	//
	// m_And does not commute, which is why a second spelling was written out.
	// It bound op_lshr1 and left op_add null, and the very next line matches
	// against op_add -- dyn_cast on a null Value. m_c_And is the commuting
	// matcher and binds the same two operands either way round.
	if (!match(op_and, m_c_And(m_Value(op_add), m_ConstantInt(op_n)))) return nullptr;

	if (! match(op_add, m_Add(m_Value(op_lshr1), m_Value(op_x_tmp))))
		return nullptr;

	// are we still using X?
	if (op_x_tmp != op_x) {
		if (op_lshr1 == op_x)
			// we have checked for X, X can be discarded
			op_lshr1 = op_x_tmp;
	}

	if (! match(op_lshr1, m_LShr(m_Value(op_ashr1), m_ConstantInt(cnst))))
		return nullptr;

	if (! match(op_ashr1, m_AShr(m_Value(op_x_tmp), m_ConstantInt(cnst)))
			&& *cnst->getValue().getRawData() != 31)
		return nullptr;

	if (op_x_tmp != op_x)
		return nullptr;

	// The modulus is the mask plus one, computed at the value's own width. An
	// all-ones mask makes that zero, and `srem x, 0` is undefined behaviour,
	// so it has to be checked before anything is erased -- once
	// eraseInstFromBasicBlock has run the operands are undef and there is no
	// way to decline the rewrite.
	llvm::APInt modulus = op_n->getValue().zextOrTrunc(op_x->getType()->getIntegerBitWidth()) + 1;
	if (!isUsableDivisor(modulus.getSExtValue())) return nullptr;

	// now exchange the idiom
	eraseInstFromBasicBlock(op_and, val.getParent());
	eraseInstFromBasicBlock(op_ashr, val.getParent());
	eraseInstFromBasicBlock(op_add, val.getParent());
	eraseInstFromBasicBlock(op_lshr, val.getParent());
	eraseInstFromBasicBlock(op_lshr1, val.getParent());
	eraseInstFromBasicBlock(op_ashr1, val.getParent());

	Constant* NewCst = ConstantInt::get(op_x->getType(), modulus);
	return BinaryOperator::CreateSRem(op_x, NewCst);
}

/**
 * Stage 11: Integer absolute value idiom.
 * CDQ; XOR eax, edx; SUB eax, edx → abs(x)
 * Pattern: sub(xor(x, ashr(x, w-1)), ashr(x, w-1))
 */
Instruction * IdiomsCommon::exchangeIntegerAbs(BasicBlock::iterator iter) const
{
	Instruction & val = (*iter);
	Value * op_xor = nullptr;
	Value * op_ashr = nullptr;
	Value * op_x = nullptr;
	ConstantInt * cnst = nullptr;

	// The subtrahend and the xor's other operand have to be the same value.
	// `op_ashr` was bound here from the sub and then REBOUND by the xor match
	// below, so the sub's right-hand side was never looked at again:
	// `(x ^ (x s>> 31)) - y` was rewritten as `abs(x)` for any y. For x = 5 and
	// y = 100 the original is -95 and the rewrite was 5.
	Value* op_sub_rhs = nullptr;
	if (!match(&val, m_Sub(m_Value(op_xor), m_Value(op_sub_rhs)))) return nullptr;

	if (!match(op_xor, m_c_Xor(m_Value(op_x), m_Value(op_ashr)))) return nullptr;

	if (op_ashr != op_sub_rhs)
	{
		// m_c_Xor may have bound the pair the other way round.
		std::swap(op_x, op_ashr);
		if (op_ashr != op_sub_rhs) return nullptr;
	}

	// op_ashr must be ashr(x, bitwidth-1)
	if (! match(op_ashr, m_AShr(m_Value(), m_ConstantInt(cnst))))
		return nullptr;

	unsigned width = op_x->getType()->getIntegerBitWidth();
	if (width != 32 && width != 64)
		return nullptr;

	unsigned shiftAmt = width - 1;
	if (cnst->getZExtValue() != shiftAmt)
		return nullptr;

	// Verify op_ashr's operand is op_x
	Value * ashrOp0 = cast<Instruction>(op_ashr)->getOperand(0);
	if (ashrOp0 != op_x)
		return nullptr;

	eraseInstFromBasicBlock(op_xor, val.getParent());
	eraseInstFromBasicBlock(op_ashr, val.getParent());

	Constant * zero = ConstantInt::get(op_x->getType(), 0);
	Instruction * cmp = CmpInst::Create(Instruction::ICmp, ICmpInst::ICMP_SLT, op_x, zero, "abs.cmp");
	BinaryOperator * neg = BinaryOperator::CreateSub(zero, op_x, "abs.neg");
	cmp->insertBefore(iter);
	neg->insertBefore(iter);
	return SelectInst::Create(cmp, neg, op_x, "abs");
}

} // namespace bin2llvmir
} // namespace retdec
