/**
 * @file src/sort_detect/bubblesort_detect.cpp
 * @brief Bubble sort detector — nested-loop adjacent swap pattern.
 *
 * Bubble sort has element comparisons and swaps but no recursion and no
 * radix-style digit extraction.  Distinguished from radix sort by the
 * presence of multiple Compare instructions in nested loop structure.
 */

#include "retdec/sort_detect/sort_detect.h"
#include "retdec/ssa/ssa.h"
#include <algorithm>
#include <cstdio>
#include <cstdlib>

namespace retdec {
namespace sort_detect {

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

static bool addrIsStack(const ssa::SSAFunction& fn, const ssa::IrInstr& i)
{
	const uint32_t addrOperand = i.op == ssa::IrInstr::Op::Store ? 1 : 0;
	for (const auto& u: i.uses)
		if (u.operandIndex == addrOperand)
			if (const auto* a = fn.value(u.valueId); a && a->isStackSlot) return true;
	return false;
}

// A swap writes two elements, each with a value loaded from memory.  Plain
// store counting also matched -O0 code, where every block spills to the stack.
static bool hasSwapInBlock(const ssa::SSAFunction& fn, const ssa::BasicBlock& blk)
{
	int moves = 0;
	for (const auto* st: blk.instrs)
	{
		if (!st || st->op != ssa::IrInstr::Op::Store || addrIsStack(fn, *st)) continue;
		for (const auto& u: st->uses)
		{
			if (u.operandIndex != 0) continue;
			const auto* v = fn.value(u.valueId);
			const auto* ld = v ? v->defInstr : nullptr;
			if (ld && ld->op == ssa::IrInstr::Op::Load && !addrIsStack(fn, *ld))
			{
				++moves;
				break;
			}
		}
	}
	return moves >= 2;
}

static const ssa::IrInstr* defOf(const ssa::SSAFunction& fn, ssa::ValueId id)
{
	const auto* v = fn.value(id);
	return v ? v->defInstr : nullptr;
}

/// Skip masks: `(x & 0xffffffff)` is still x for this purpose.
static const ssa::IrInstr* skipMask(const ssa::SSAFunction& fn, const ssa::IrInstr* d)
{
	while (d && d->op == ssa::IrInstr::Op::And && !d->uses.empty())
	{
		const ssa::IrInstr* next = nullptr;
		for (const auto& u: d->uses)
			if (const auto* x = defOf(fn, u.valueId)) next = x;
		d = next;
	}
	return d;
}

/// The memory load a half-word shift reads, if `d` is `load >> 32` or
/// `load << 32` (under masks).
static const ssa::IrInstr* halfShiftedLoad(const ssa::SSAFunction& fn, const ssa::IrInstr* d, ssa::IrInstr::Op op)
{
	d = skipMask(fn, d);
	if (!d || d->op != op) return nullptr;
	bool by32 = false;
	const ssa::IrInstr* src = nullptr;
	for (const auto& u: d->uses)
	{
		const auto* v = fn.value(u.valueId);
		if (v && v->kind == ssa::ValueKind::Immediate && v->imm == 32) by32 = true;
		else if (const auto* x = skipMask(fn, defOf(fn, u.valueId)); x && x->op == ssa::IrInstr::Op::Load) src = x;
	}
	return by32 && src && !addrIsStack(fn, *src) ? src : nullptr;
}

// -O2 loads two adjacent 32-bit elements as one 64-bit word and swaps them by
// writing back the word with its halves exchanged:
//   w = *p; if (hi(w) < lo(w)) *p = (w >> 32) | (w << 32);
static bool hasPackedPairSwap(const ssa::SSAFunction& fn)
{
	for (uint32_t b = 0; b < fn.blockCount(); ++b)
	{
		const auto* blk = fn.block(b);
		if (!blk) continue;
		for (const auto* st: blk->instrs)
		{
			if (!st || st->op != ssa::IrInstr::Op::Store || addrIsStack(fn, *st)) continue;
			for (const auto& u: st->uses)
			{
				if (u.operandIndex != 0) continue;
				const auto* orI = defOf(fn, u.valueId);
				if (!orI || orI->op != ssa::IrInstr::Op::Or) continue;
				const ssa::IrInstr *hi = nullptr, *lo = nullptr;
				for (const auto& o: orI->uses)
				{
					const auto* x = defOf(fn, o.valueId);
					if (const auto* l = halfShiftedLoad(fn, x, ssa::IrInstr::Op::Shr)) hi = l;
					if (const auto* l = halfShiftedLoad(fn, x, ssa::IrInstr::Op::Shl)) lo = l;
				}
				if (hi && hi == lo) return true;
			}
		}
	}
	return false;
}

static bool hasSwapPattern(const ssa::SSAFunction& fn)
{
	for (uint32_t b = 0; b < fn.blockCount(); ++b)
	{
		const auto* blk = fn.block(b);
		if (blk && hasSwapInBlock(fn, *blk)) return true;
	}
	return hasPackedPairSwap(fn);
}

static int countPhis(const ssa::SSAFunction& fn)
{
	int n = 0;
	for (const auto& phi: fn.phis())
		if (phi) ++n;
	return n;
}

static int countSelfCalls(const ssa::SSAFunction& fn)
{
	int n = 0;
	const std::string& name = fn.name();
	for (uint32_t b = 0; b < fn.blockCount(); ++b)
	{
		const auto* blk = fn.block(b);
		if (!blk) continue;
		for (const auto* instr: blk->instrs)
			if (instr && instr->op == ssa::IrInstr::Op::Call && instr->calleeName == name) ++n;
	}
	return n;
}

} // anonymous namespace

SortResult BubbleSortDetector::detect(const ssa::SSAFunction& fn) const
{
	SortResult result;
	result.algorithm = SortAlgorithm::BubbleSort;

	// Suppress only on partition evidence that actually distinguishes a
	// partition loop from a bubble sort.  The score gate alone was vacuous:
	// this detector's own entry conditions (>= 3 compares, a swap, >= 3
	// conditional branches) already put PartitionFingerprint at 0.75, past the
	// 0.45 gate, so every bubble sort returned confidence 0 here and was
	// reported as introsort instead — a false positive for introsort and a
	// false negative for bubble sort at the same time.  The converging-index
	// phis are the part of the partition fingerprint a bubble sort cannot
	// reproduce.
	PartitionFingerprint pf;
	const auto part = pf.analyse(fn);
	// The converging-index test lives on PartitionFingerprint, where every
	// consumer gets it: this file used to carry a private, weaker copy while
	// the shared predicate's answer sat unread on the evidence as isHoareStyle.
	const bool diag = std::getenv("RETDEC_SORT_DIAG");
	if (diag) std::fprintf(stderr, "BUBBLE %s part=%d/%.2f/%d\n", fn.name().c_str(), part.found, part.confidence, part.isHoareStyle);
	if (part.found && part.confidence >= 0.45f && part.isHoareStyle) return result;

	// Same mistake, second guard: SiftDownEvidence.found needs two of its
	// three signals, and two of them — a compare feeding a conditional branch,
	// and a two-store swap — are exactly what a bubble sort has.  Only the
	// child-index arithmetic (2*i+1, i.e. Shl by 1 / Mul by 2) is heap-specific,
	// so require it before letting sift-down evidence veto a bubble sort.
	SiftDownFingerprint sdf;
	const auto sift = sdf.analyse(fn);
	if (sift.found && sift.hasLeftArith) return result;

	const int cmps = countOp(fn, ssa::IrInstr::Op::Compare);
	const int cb = countOp(fn, ssa::IrInstr::Op::CondBranch);
	// At -O0 the loop counters live in stack slots, so there are no phis;
	// two retreating edges show the same nested-loop shape.
	const int loops = static_cast<int>(ssa::retreatingEdges(fn).size());
	const int phis = std::max(countPhis(fn), loops);
	const bool swap = hasSwapPattern(fn);
	const int self = countSelfCalls(fn);

	if (diag) std::fprintf(stderr, "BUBBLE %s sift=%d/%d cmps=%d cb=%d phis=%d swap=%d self=%d\n", fn.name().c_str(), sift.found, sift.hasLeftArith, cmps, cb, phis, swap, self);
	if (cmps < 3 || !swap || self > 0 || cb < 3 || phis < 2) return result;

	float score = 0.0f;
	if (cmps >= 3) score += 0.30f;
	if (swap) score += 0.25f;
	if (cb >= 3) score += 0.25f;
	if (phis >= 2) score += 0.20f;

	result.confidence = score > 1.0f ? 1.0f : score;
	result.compilerVariant = CompilerVariant::Unknown;
	return result;
}

} // namespace sort_detect
} // namespace retdec
