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

// Power-of-two wrap: And with (2^k-1). Div immediates are not wrap
// (B8 FP 0.700 when strength-reduction Div was accepted). Rem with a
// capacity immediate is wrap (`i % n`).
//
// The lower bound used to be "not zero", which accepted 1 and 3 -- so `flags &
// 1`, the commonest bit test there is, read as a two-entry ring wrap. A ring
// of fewer than eight entries is not something a mask can be told apart from
// an ordinary flag test or an alignment check, and the sibling detector's
// isBucketMask draws the same kind of line at 15.
static bool isWrapMask(uint64_t imm)
{
	if (imm < 7ULL || imm > 0xffffULL) return false;
	return ((imm + 1ULL) & imm) == 0ULL;
}

// A ring buffer is read or written more than once from the same code.
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

static bool isWrapCapacity(uint64_t imm)
{
	return imm >= 2 && imm <= 0xffffULL;
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
			if (instr->op == ssa::IrInstr::Op::And)
			{
				for (const auto& use: instr->uses)
				{
					const auto* val = fn.value(use.valueId);
					if (val && val->kind == ssa::ValueKind::Immediate && isWrapMask(val->imm)) return true;
				}
			}
			else if (instr->op == ssa::IrInstr::Op::Rem)
			{
				for (const auto& use: instr->uses)
				{
					const auto* val = fn.value(use.valueId);
					if (val && val->kind == ssa::ValueKind::Immediate && isWrapCapacity(val->imm)) return true;
				}
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

	// A wrap index that is never revisited is an array index, not a ring: the
	// buffer is a ring because the producer or the consumer comes back round.
	// OpenAddressingDetector, whose evidence is the same shape, requires this
	// too. Without it a single masked load and store in straight-line code was
	// a ring buffer.
	if (!hasBackEdge(fn)) return result;
	if (!mod || loads < 1 || stores < 1 || hasInlineHash(fn)) return result;

	// Graded, not pinned. The floor used to be 0.35 + 0.25 + 0.25 = 0.85 the
	// moment the guard above passed, and any Compare took it to 1.00 -- which
	// made the `confidence < 0.45f` test below unreachable and, because
	// ContainerDetector::analyseFunction keeps the highest-confidence answer,
	// silently overrode every other detector on the same function.
	float score = 0.0f;
	if (mod) score += 0.30f;
	score += 0.10f * static_cast<float>(loads < 2 ? loads : 2);
	score += 0.10f * static_cast<float>(stores < 2 ? stores : 2);
	if (cmps >= 1) score += 0.10f;
	if (cmps >= 2) score += 0.10f;

	result.confidence = score > 1.0f ? 1.0f : score;
	if (result.confidence < 0.45f) return result;

	result.emittedType = "ring_buffer";
	result.elementType.kind = RecoveredType::Kind::Int8;
	return result;
}

} // namespace container_detect
} // namespace retdec
