/**
 * @file src/crypto_detect/arx_rotate.h
 * @brief Recognising a rotation in the IR, for the ARX cipher detectors.
 *
 * ChaCha20 and Salsa20 are ARX constructions: add, rotate, xor. Their detectors
 * scored the rotation *amounts* -- 16, 12, 8, 7 for ChaCha20; 7, 9, 13, 18 for
 * Salsa20 -- by asking whether the immediate appeared anywhere in the function,
 * and asked for the rotation itself only as `>= 1 Add && >= 1 Xor && (>= 1 Shl
 * || >= 1 Or)`. None of those is a rotation, and all of those numbers are
 * ordinary. An everyday byte-mixing string hash -- two loads, a xor, a mask by
 * 255, a shift left by 8, a shift right by 7, two adds -- therefore reported
 * ChaCha20 at 0.50 and Salsa20 at 0.50, each with an annotation emitted into
 * the decompiled C.
 *
 * A rotate is a specific thing, and the IR can say so: either a Rol/Ror opcode,
 * or the two halves a compiler emits when it has none -- a shift one way by k
 * and a shift the other way by width - k, recombined with an Or. That is what
 * these ask for.
 *
 * What they do not ask is that the two shifts act on the same value. Nothing
 * here can: on the production path the SSA carries no def-use edges at all (see
 * docs/internal/UNFIXED_AUDIT_FINDINGS.md section 5). Complementary shift
 * amounts plus the recombining Or is the strongest statement available, and it
 * is a great deal stronger than an immediate appearing somewhere.
 */

#ifndef RETDEC_CRYPTO_DETECT_ARX_ROTATE_H
#define RETDEC_CRYPTO_DETECT_ARX_ROTATE_H

#include "retdec/ssa/ssa.h"

#include <cstdint>

namespace retdec {
namespace crypto_detect {
namespace arx {

/// True when @p op appears with @p amount as an immediate operand.
inline bool hasShiftBy(const ssa::SSAFunction& fn, ssa::IrInstr::Op op, uint64_t amount)
{
	for (uint32_t b = 0; b < fn.blockCount(); ++b)
	{
		const auto* blk = fn.block(b);
		if (!blk) continue;
		for (const auto* instr: blk->instrs)
		{
			if (!instr || instr->op != op) continue;
			for (const auto& use: instr->uses)
			{
				const auto* val = fn.value(use.valueId);
				if (val && val->kind == ssa::ValueKind::Immediate && val->imm == amount) return true;
			}
		}
	}
	return false;
}

/// True when @p op appears at all.
inline bool hasOp(const ssa::SSAFunction& fn, ssa::IrInstr::Op op)
{
	for (uint32_t b = 0; b < fn.blockCount(); ++b)
	{
		const auto* blk = fn.block(b);
		if (!blk) continue;
		for (const auto* instr: blk->instrs)
			if (instr && instr->op == op) return true;
	}
	return false;
}

/// True when the function rotates a @p width -bit value by @p k.
///
/// Either a Rol/Ror carrying k or width - k, or the shift pair a compiler
/// without a rotate opcode emits: `(x << k) | (x >> (width - k))`, in either
/// direction, with the Or that puts the halves back together.
inline bool hasRotateBy(const ssa::SSAFunction& fn, uint64_t k, uint64_t width = 32)
{
	if (k == 0 || k >= width) return false;
	const uint64_t co = width - k;

	if (hasShiftBy(fn, ssa::IrInstr::Op::Rol, k) || hasShiftBy(fn, ssa::IrInstr::Op::Rol, co)
		|| hasShiftBy(fn, ssa::IrInstr::Op::Ror, k) || hasShiftBy(fn, ssa::IrInstr::Op::Ror, co))
		return true;

	if (!hasOp(fn, ssa::IrInstr::Op::Or)) return false;

	const bool leftK = hasShiftBy(fn, ssa::IrInstr::Op::Shl, k);
	const bool rightC = hasShiftBy(fn, ssa::IrInstr::Op::Shr, co) || hasShiftBy(fn, ssa::IrInstr::Op::Sar, co);
	const bool leftC = hasShiftBy(fn, ssa::IrInstr::Op::Shl, co);
	const bool rightK = hasShiftBy(fn, ssa::IrInstr::Op::Shr, k) || hasShiftBy(fn, ssa::IrInstr::Op::Sar, k);

	return (leftK && rightC) || (leftC && rightK);
}

/// True when the function has the add-rotate-xor shape at all: an Add, a Xor
/// and at least one of the given rotations.
///
/// The rotation is the part that was missing. `>= 1 Shl || >= 1 Or` is not a
/// rotation and does not distinguish an ARX round from any integer arithmetic.
inline bool hasAddRotateXor(const ssa::SSAFunction& fn, const uint64_t* amounts, std::size_t count, uint64_t width = 32)
{
	if (!hasOp(fn, ssa::IrInstr::Op::Add)) return false;
	if (!hasOp(fn, ssa::IrInstr::Op::Xor)) return false;
	for (std::size_t i = 0; i < count; ++i)
		if (hasRotateBy(fn, amounts[i], width)) return true;
	return false;
}

} // namespace arx
} // namespace crypto_detect
} // namespace retdec

#endif // RETDEC_CRYPTO_DETECT_ARX_ROTATE_H
