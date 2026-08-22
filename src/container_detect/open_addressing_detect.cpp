/**
 * @file src/container_detect/open_addressing_detect.cpp
 * @brief Open-addressing hash table detector (linear probing).
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

static bool hasBackEdge(const ssa::SSAFunction& fn)
{
	for (uint32_t b = 0; b < fn.blockCount(); ++b)
	{
		const auto* blk = fn.block(b);
		if (!blk) continue;
		for (uint32_t s: blk->succs)
			if (s <= b) return true;
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

static bool hasHashSignal(const ssa::SSAFunction& fn)
{
	if (hasInlineHash(fn)) return true;
	for (uint32_t b = 0; b < fn.blockCount(); ++b)
	{
		const auto* blk = fn.block(b);
		if (!blk) continue;
		for (const auto* instr: blk->instrs)
		{
			if (!instr || instr->op != ssa::IrInstr::Op::Call) continue;
			const auto& cn = instr->calleeName;
			if (cn.find("hash") != std::string::npos || cn.find("strcmp") != std::string::npos) return true;
		}
	}
	return false;
}

// At least 16 buckets. And 7 (strlen align), rem 8 (AES rotate),
// rem 256 / And 255 (byte wrap) are not a table. Bare Div is not either.
static bool isBucketMask(uint64_t imm)
{
	if (imm < 15ULL || imm > 0xffffULL) return false;
	if (imm == 255ULL || imm == 65535ULL) return false;
	return ((imm + 1ULL) & imm) == 0ULL;
}

static bool isBucketCapacity(uint64_t imm)
{
	if (imm < 16ULL || imm > 65536ULL) return false;
	if (imm == 256ULL) return false;
	return (imm & (imm - 1ULL)) == 0ULL;
}

static bool hasModuloIndex(const ssa::SSAFunction& fn)
{
	for (uint32_t b = 0; b < fn.blockCount(); ++b)
	{
		const auto* blk = fn.block(b);
		if (!blk) continue;
		for (const auto* instr: blk->instrs)
		{
			if (!instr) continue;
			if (instr->op != ssa::IrInstr::Op::And && instr->op != ssa::IrInstr::Op::Div) continue;
			for (const auto& u: instr->uses)
			{
				const auto* iv = fn.value(u.valueId);
				if (!iv || iv->kind != ssa::ValueKind::Immediate) continue;
				if (instr->op == ssa::IrInstr::Op::And && isBucketMask(iv->imm)) return true;
				if (instr->op == ssa::IrInstr::Op::Div && isBucketCapacity(iv->imm)) return true;
			}
		}
	}
	return false;
}

} // anonymous namespace

ContainerResult OpenAddressingDetector::detect(const ssa::SSAFunction& fn) const
{
	ContainerResult result;
	result.kind = ContainerKind::Array;

	if (!hasBackEdge(fn)) return result;

	const bool hash = hasHashSignal(fn);
	const bool mod = hasModuloIndex(fn);
	const int cmps = countOp(fn, ssa::IrInstr::Op::Compare);
	const int loads = countOp(fn, ssa::IrInstr::Op::Load);
	const int stores = countOp(fn, ssa::IrInstr::Op::Store);

	if (!hash || !mod || cmps < 1 || loads < 2) return result;

	float score = 0.0f;
	if (hash) score += 0.35f;
	if (mod) score += 0.30f;
	if (cmps >= 1) score += 0.20f;
	if (loads >= 2) score += 0.15f;
	if (stores >= 1) score += 0.10f;

	result.confidence = score > 1.0f ? 1.0f : score;
	if (result.confidence < 0.55f) return result;

	result.emittedType = "open_addressing_hash_table";
	result.elementType.kind = RecoveredType::Kind::Int32;
	result.keyType.kind = RecoveredType::Kind::Int32;
	return result;
}

} // namespace container_detect
} // namespace retdec
