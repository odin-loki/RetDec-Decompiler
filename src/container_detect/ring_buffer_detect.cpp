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

static bool isWrapInstr(const ssa::SSAFunction& fn, const ssa::IrInstr& instr);

// A ring keeps its wrapped head or tail: `r->tail = (r->tail + 1) % N` stores
// the wrapped index back to memory that outlives the call.  A linear probe in
// an open-addressing table wraps the same way but keeps the index in a local,
// and a wrapped index that is only used to address an element is an array
// index.  Requiring the persisted wrap replaces a loop requirement, which
// both missed push/pop (no loop) and passed every probing loop.
static bool hasPersistedWrap(const ssa::SSAFunction& fn)
{
	for (uint32_t b = 0; b < fn.blockCount(); ++b)
	{
		const auto* blk = fn.block(b);
		if (!blk) continue;
		for (const auto* st: blk->instrs)
		{
			if (!st || st->op != ssa::IrInstr::Op::Store) continue;
			bool wrappedValue = false, stackAddr = false;
			for (const auto& u: st->uses)
			{
				const auto* v = fn.value(u.valueId);
				if (!v) continue;
				if (u.operandIndex == 0 && v->defInstr && isWrapInstr(fn, *v->defInstr)) wrappedValue = true;
				if (u.operandIndex == 1 && v->isStackSlot) stackAddr = true;
			}
			if (wrappedValue && !stackAddr) return true;
		}
	}
	return false;
}

static bool isWrapCapacity(uint64_t imm)
{
	return imm >= 2 && imm <= 0xffffULL;
}

static bool isWrapInstr(const ssa::SSAFunction& fn, const ssa::IrInstr& instr)
{
	if (instr.op != ssa::IrInstr::Op::And && instr.op != ssa::IrInstr::Op::Rem) return false;
	for (const auto& use: instr.uses)
	{
		const auto* val = fn.value(use.valueId);
		if (!val || val->kind != ssa::ValueKind::Immediate) continue;
		if (instr.op == ssa::IrInstr::Op::And ? isWrapMask(val->imm) : isWrapCapacity(val->imm)) return true;
	}
	return false;
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

	// A wrap index that is only used, never kept, is an array index or a
	// probe, not a ring; see hasPersistedWrap.
	if (!hasPersistedWrap(fn)) return result;
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
