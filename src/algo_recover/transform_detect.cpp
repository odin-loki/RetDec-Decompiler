/**
 * @file src/algo_recover/transform_detect.cpp
 * @brief std::transform detector — source→destination one-to-one loop.
 *
 * ## Structural invariant
 *
 * A compiled std::transform (or its manual equivalent) has:
 *
 *   1. A loop with one Load from a source iterator and one Store to a
 *      destination iterator, both incremented by the same stride each iteration.
 *   2. The value stored is derived from the value loaded (one-to-one mapping).
 *   3. The source and destination pointers have different base values (otherwise
 *      this is an in-place map, which we still accept but note).
 *   4. An inlined lambda / function call appears between the Load and Store when
 *      the transform is not identity (std::copy).
 *
 * ## Differentiating from std::copy
 *
 * std::copy is transform with identity function: stored value == loaded value.
 * We emit it as `std::copy` when there is no intervening computation.
 *
 * ## Back-inserter pattern
 *
 * When the destination is not a random-access range but a container's
 * push_back (output via `std::back_inserter`), the Store is replaced by a
 * Call to `push_back` / `_M_insert` / `emplace_back`.
 *
 * ## Confidence scoring
 *
 *   Loop with back-edge              required
 *   Load in loop body                +0.25
 *   Store in loop body               +0.25
 *   Both src and dst ptrs advanced   +0.25
 *   Derived store value (not raw)    +0.15 (lambda/call)
 *   Back-inserter call               +0.10 (replaces Store signal)
 */

#include "retdec/algo_recover/algo_recover.h"
#include "retdec/ssa/ssa.h"

#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <algorithm>

namespace retdec {
namespace algo_recover {

namespace {

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

static int countOp(const ssa::SSAFunction& fn, ssa::IrInstr::Op op)
{
	int n = 0;
	for (uint32_t b = 0; b < fn.blockCount(); ++b)
	{
		const auto* blk = fn.block(b);
		if (!blk) continue;
		for (const auto* i: blk->instrs)
			if (i && i->op == op) ++n;
	}
	return n;
}

// Both source and destination pointers advanced by Add in loop body.
static bool hasTwoPtrsAdvanced(const ssa::SSAFunction& fn)
{
	return countOp(fn, ssa::IrInstr::Op::Add) >= 2;
}

// Derived store: a Call or any non-trivial computation between Load and Store.
// Note: 2 Adds are expected for src/dst pointer advances; require 3+ for computation.
static bool hasDerivedStore(const ssa::SSAFunction& fn)
{
	return countOp(fn, ssa::IrInstr::Op::Call) >= 1 || countOp(fn, ssa::IrInstr::Op::Add) >= 3
		|| countOp(fn, ssa::IrInstr::Op::Mul) >= 1 || countOp(fn, ssa::IrInstr::Op::Shl) >= 1;
}

// Back-inserter: push_back / emplace_back / _M_insert call in loop.
static bool hasBackInserterCall(const ssa::SSAFunction& fn)
{
	for (uint32_t b = 0; b < fn.blockCount(); ++b)
	{
		const auto* blk = fn.block(b);
		if (!blk) continue;
		for (const auto* i: blk->instrs)
		{
			if (!i || i->op != ssa::IrInstr::Op::Call) continue;
			const auto& cn = i->calleeName;
			if (cn.find("push_back") != std::string::npos || cn.find("emplace_back") != std::string::npos
				|| cn.find("_M_insert") != std::string::npos || cn.find("append") != std::string::npos)
				return true;
		}
	}
	return false;
}

// Dataflow check, when the operands are wired: does some Store write a value
// computed from a Load of a *different* address? That is dst[i] = f(src[i]).
// A loop over register spills -- `i = *slot; *slot = i + 1` -- loads and
// stores only the same slot, and a call result stored to a spill slot is not
// derived from any load, so neither is a transform.
//
// Returns Unknown when no Store has its (value, address) operands, as in the
// hand-built shapes, so the absence of operands is never read as evidence.
enum class Flow { Unknown, Yes, No };

static Flow loadFlowsToOtherStore(const ssa::SSAFunction& fn)
{
	std::unordered_map<ssa::ValueId, const ssa::IrInstr*> def;
	std::vector<const ssa::IrInstr*> stores;
	for (uint32_t b = 0; b < fn.blockCount(); ++b)
	{
		const auto* blk = fn.block(b);
		if (!blk) continue;
		for (const auto* i: blk->instrs)
		{
			if (!i) continue;
			if (i->defValue != ssa::kInvalidValue) def[i->defValue] = i;
			if (i->op == ssa::IrInstr::Op::Store && i->uses.size() >= 2) stores.push_back(i);
		}
	}
	if (stores.empty()) return Flow::Unknown;

	for (const auto* st: stores)
	{
		const ssa::ValueId addr = st->uses[1].valueId;
		std::vector<std::pair<ssa::ValueId, int>> work{{st->uses[0].valueId, 0}};
		std::unordered_set<ssa::ValueId> seen;
		while (!work.empty())
		{
			auto [v, depth] = work.back();
			work.pop_back();
			if (!seen.insert(v).second || depth > 8) continue;
			auto it = def.find(v);
			if (it == def.end()) continue;
			const auto* d = it->second;
			if (d->op == ssa::IrInstr::Op::Load)
			{
				if (!d->uses.empty() && d->uses[0].valueId != addr) return Flow::Yes;
				continue;
			}
			for (const auto& u: d->uses)
				work.push_back({u.valueId, depth + 1});
		}
	}
	return Flow::No;
}

static bool addrIsStack(const ssa::SSAFunction& fn, const ssa::IrInstr& i)
{
	const uint32_t addrOperand = i.op == ssa::IrInstr::Op::Store ? 1 : 0;
	for (const auto& u: i.uses)
		if (u.operandIndex == addrOperand)
			if (const auto* a = fn.value(u.valueId); a && a->isStackSlot) return true;
	return false;
}

// An identity copy moves an element: a Store to memory that is not a stack
// slot, of a value loaded (possibly through a width cast) from memory that is
// not a stack slot either.  -O0 loops that only update locals -- a popcount,
// a Fibonacci step, a varint shift -- spill to the stack and never move one.
// Unknown when no Store carries its operands (hand-built shapes).
static Flow hasElementMove(const ssa::SSAFunction& fn)
{
	bool wired = false;
	for (uint32_t b = 0; b < fn.blockCount(); ++b)
	{
		const auto* blk = fn.block(b);
		if (!blk) continue;
		for (const auto* st: blk->instrs)
		{
			if (!st || st->op != ssa::IrInstr::Op::Store || st->uses.size() < 2) continue;
			wired = true;
			if (addrIsStack(fn, *st)) continue;
			const auto* v = fn.value(st->uses[0].valueId);
			const ssa::IrInstr* d = v ? v->defInstr : nullptr;
			for (int hops = 0; d && d->op == ssa::IrInstr::Op::Assign && hops < 2 && !d->uses.empty(); ++hops)
			{
				const auto* src = fn.value(d->uses[0].valueId);
				d = src ? src->defInstr : nullptr;
			}
			if (d && d->op == ssa::IrInstr::Op::Load && !addrIsStack(fn, *d)) return Flow::Yes;
		}
	}
	return wired ? Flow::No : Flow::Unknown;
}

} // anonymous namespace

TransformEvidence TransformDetector::analyse(const ssa::SSAFunction& fn) const
{
	TransformEvidence ev;
	if (!hasBackEdge(fn)) return ev;

	// B8 HTTP-verb: many CondBranches (state machine) and one pointer
	// increment is not std::copy. memcpy-style loops have one bound
	// branch and several Adds (src+i, dst+i, i++).
	if (countOp(fn, ssa::IrInstr::Op::CondBranch) >= 3 && countOp(fn, ssa::IrInstr::Op::Add) < 3) return ev;

	const bool indexedCopy = countOp(fn, ssa::IrInstr::Op::Load) >= 1 && countOp(fn, ssa::IrInstr::Op::Store) >= 1
						  && countOp(fn, ssa::IrInstr::Op::Compare) >= 1 && countOp(fn, ssa::IrInstr::Op::Add) >= 1
						  && countOp(fn, ssa::IrInstr::Op::Call) == 0;

	ev.hasSrcDstLoad = countOp(fn, ssa::IrInstr::Op::Load) >= 1 && countOp(fn, ssa::IrInstr::Op::Store) >= 1;
	ev.hasTwoPtrsAdvanced = hasTwoPtrsAdvanced(fn) || indexedCopy;
	ev.hasNoReorder = countOp(fn, ssa::IrInstr::Op::Call) <= 1; // no sort call
	ev.hasLambdaCall = hasDerivedStore(fn) && !indexedCopy;
	ev.hasBackInserter = hasBackInserterCall(fn);
	ev.found = ev.hasSrcDstLoad;
	if (loadFlowsToOtherStore(fn) == Flow::No) return TransformEvidence{};
	ev.confidence = score(ev);
	return ev;
}

float TransformDetector::score(const TransformEvidence& ev) const
{
	if (!ev.hasSrcDstLoad) return 0.0f;
	float s = 0.0f;
	if (ev.hasSrcDstLoad) s += 0.25f;
	if (ev.hasTwoPtrsAdvanced) s += 0.30f;
	if (ev.hasNoReorder) s += 0.10f;
	if (ev.hasLambdaCall) s += 0.30f;
	if (ev.hasBackInserter) s += 0.10f;
	// Identity indexed copy (memcpy-style) without extra computation.
	if (!ev.hasLambdaCall && !ev.hasBackInserter && ev.hasTwoPtrsAdvanced) s += 0.20f;
	return s > 1.0f ? 1.0f : s;
}

std::string TransformDetector::emit(const TransformEvidence& ev, EmissionTier tier) const
{
	if (tier == EmissionTier::Low) return "for (auto src = first; src != last; ++src, ++dst) *dst = f(*src);";

	// Determine whether identity (copy) or true transform.
	bool isIdentity = !ev.hasLambdaCall && !ev.hasBackInserter;

	if (tier == EmissionTier::Medium)
	{
		if (isIdentity) return "/* std::copy? */ for (...) *dst++ = *src++;";
		return "/* std::transform? */ for (...) *dst++ = f(*src++);";
	}

	// High tier.
	if (isIdentity)
		return ev.hasBackInserter ? "std::copy(first, last, std::back_inserter(dst));" : "std::copy(first, last, dst);";
	return ev.hasBackInserter ? "std::transform(first, last, std::back_inserter(dst), f);"
							  : "std::transform(first, last, dst, f);";
}

AlgorithmResult TransformDetector::detect(const ssa::SSAFunction& fn) const
{
	AlgorithmResult result;
	result.kind = AlgorithmKind::Transform;

	auto ev = analyse(fn);
	result.confidence = ev.confidence;
	result.hasLambda = ev.hasLambdaCall;
	result.hasBackInserter = ev.hasBackInserter;

	// Assign tier (caller will override; set tentative here).
	EmissionTier tier = EmissionTier::Low;
	if (ev.confidence >= 0.75f)
		tier = EmissionTier::High;
	else if (ev.confidence >= 0.45f)
		tier = EmissionTier::Medium;
	result.tier = tier;

	if (ev.confidence < 0.01f) return result;

	// Identity transform → emit as copy. Mul/Xor means AES/atoi/DFS,
	// not memcpy. True transform (lambda/call) still uses those ops.
	if (!ev.hasLambdaCall && !ev.hasBackInserter)
	{
		if (countOp(fn, ssa::IrInstr::Op::Mul) >= 1 || countOp(fn, ssa::IrInstr::Op::Xor) >= 1)
		{
			result.confidence = 0.0f;
			result.kind = AlgorithmKind::Transform;
			return result;
		}
		if (hasElementMove(fn) == Flow::No)
		{
			result.confidence = 0.0f;
			return result;
		}
		result.kind = AlgorithmKind::Copy;
	}

	result.emittedForm = emit(ev, tier);
	return result;
}

} // namespace algo_recover
} // namespace retdec
