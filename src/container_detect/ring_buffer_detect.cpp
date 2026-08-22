/**
 * @file src/container_detect/ring_buffer_detect.cpp
 * @brief Ring buffer detector — modulo index with array load/store.
 */

#include "retdec/container_detect/container_detect.h"
#include "retdec/ssa/ssa.h"

namespace retdec {
namespace container_detect {

namespace {

static int countOp(const ssa::SSAFunction& fn, ssa::IrInstr::Op op)
{
	int n = 0;
	for (uint32_t b = 0; b < fn.blockCount(); ++b)
	{
		const auto* blk = fn.block(b);
		if (!blk) continue;
		for (const auto* instr: blk->instrs)
			if (instr && instr->op == op) ++n;
	}
	return n;
}

// Power-of-two wrap: And with (2^k - 1). Plain Div is box-blur / n
// or heapsort n/2. Recovered SSA from llvm_to_ssa has empty uses, so
// this does not fire on decompiled ELFs (B8 box-blur FP drops; corpus
// ring_buffer is a miss until uses are attached). Stack-align And
// (0xfffffffffffffff0) is excluded by the mask bound.
static bool isWrapMask(uint64_t imm)
{
	if (imm == 0 || imm > 0xffffULL) return false;
	return ((imm + 1ULL) & imm) == 0ULL;
}

static bool hasModuloIndex(const ssa::SSAFunction& fn)
{
	for (uint32_t b = 0; b < fn.blockCount(); ++b)
	{
		const auto* blk = fn.block(b);
		if (!blk) continue;
		for (const auto* instr: blk->instrs)
		{
			if (!instr || instr->op != ssa::IrInstr::Op::And) continue;
			for (const auto& use: instr->uses)
			{
				const auto* val = fn.value(use.valueId);
				if (val && val->kind == ssa::ValueKind::Immediate && isWrapMask(val->imm)) return true;
			}
		}
	}
	return false;
}

static bool hasInlineHash(const ssa::SSAFunction& fn)
{
	bool hasXor = false, hasMul = false;
	for (uint32_t b = 0; b < fn.blockCount(); ++b)
	{
		const auto* blk = fn.block(b);
		if (!blk) continue;
		for (const auto* instr: blk->instrs)
		{
			if (!instr) continue;
			if (instr->op == ssa::IrInstr::Op::Xor) hasXor = true;
			if (instr->op == ssa::IrInstr::Op::Mul) hasMul = true;
		}
	}
	return hasXor && hasMul;
}

} // anonymous namespace

ContainerResult RingBufferDetector::detect(const ssa::SSAFunction& fn) const
{
	ContainerResult result;
	result.kind = ContainerKind::Array;

	const bool mod = hasModuloIndex(fn);
	const int loads = countOp(fn, ssa::IrInstr::Op::Load);
	const int stores = countOp(fn, ssa::IrInstr::Op::Store);
	const int cmps = countOp(fn, ssa::IrInstr::Op::Compare);

	if (!mod || loads < 1 || stores < 1 || hasInlineHash(fn)) return result;

	float score = 0.0f;
	if (mod) score += 0.35f;
	if (loads >= 1) score += 0.25f;
	if (stores >= 1) score += 0.25f;
	if (cmps >= 1) score += 0.15f;

	result.confidence = score > 1.0f ? 1.0f : score;
	if (result.confidence < 0.45f) return result;

	result.emittedType = "ring_buffer";
	result.elementType.kind = RecoveredType::Kind::Int8;
	return result;
}

} // namespace container_detect
} // namespace retdec
