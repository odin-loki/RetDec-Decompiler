/**
 * @file src/sort_detect/quicksort_detect.cpp
 * @brief Quicksort detector — partition loop without introsort depth counter.
 */

#include "retdec/sort_detect/sort_detect.h"
#include "retdec/ssa/ssa.h"

#include <cstdio>
#include <cstdlib>

namespace retdec {
namespace sort_detect {

namespace {

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

static bool hasDepthCounter(const ssa::SSAFunction& fn)
{
	bool hasSub = false;
	bool hasCmpZero = false;
	for (uint32_t b = 0; b < fn.blockCount(); ++b)
	{
		const auto* blk = fn.block(b);
		if (!blk) continue;
		for (const auto* instr: blk->instrs)
		{
			if (!instr) continue;
			if (instr->op == ssa::IrInstr::Op::Sub) hasSub = true;
			if (instr->op == ssa::IrInstr::Op::Compare)
			{
				for (const auto& use: instr->uses)
				{
					const auto* val = fn.value(use.valueId);
					if (val && val->kind == ssa::ValueKind::Immediate && val->imm == 0)
					{
						hasCmpZero = true;
						break;
					}
				}
			}
		}
	}
	return hasSub && hasCmpZero;
}

static int countOp(const ssa::SSAFunction& fn, ssa::IrInstr::Op op)
{
	int n = 0;
	for (uint32_t b = 0; b < fn.blockCount(); ++b)
		if (const auto* blk = fn.block(b))
			for (const auto* instr: blk->instrs)
				if (instr && instr->op == op) ++n;
	return n;
}

// The quicksort skeleton with the partition in a helper -- what -O0 keeps:
//
//     if (lo < hi) { p = partition(a, lo, hi); qs(a, lo, p - 1); qs(a, p + 1, hi); }
//
// No loop of its own, a call to another function first, then two self-calls.
// Mergesort recurses first and merges after, and splits at a shifted midpoint.
static bool isPartitionThenRecurse(const ssa::SSAFunction& fn)
{
	if (std::getenv("RETDEC_SORT_DIAG"))
	{
		std::fprintf(stderr, "QS %s cycle=%d shr=%d calls:", fn.name().c_str(), ssa::hasCycle(fn), countOp(fn, ssa::IrInstr::Op::Shr));
		for (uint32_t b = 0; b < fn.blockCount(); ++b)
			if (const auto* blk = fn.block(b))
				for (const auto* i: blk->instrs)
					if (i && i->op == ssa::IrInstr::Op::Call) std::fprintf(stderr, " [%u]%s", b, i->calleeName.c_str());
		std::fprintf(stderr, "\n");
	}
	if (ssa::hasCycle(fn) || countOp(fn, ssa::IrInstr::Op::Shr) > 0) return false;
	const std::string& name = fn.name();
	bool sawHelper = false;
	int selfAfterHelper = 0;
	for (uint32_t b = 0; b < fn.blockCount(); ++b)
	{
		const auto* blk = fn.block(b);
		if (!blk) continue;
		for (const auto* instr: blk->instrs)
		{
			if (!instr || instr->op != ssa::IrInstr::Op::Call) continue;
			if (instr->calleeName == name)
			{
				if (!sawHelper) return false;
				++selfAfterHelper;
			}
			else if (!sawHelper)
			{
				if (instr->calleeName.empty()) return false;
				sawHelper = true;
			}
		}
	}
	return selfAfterHelper == 2;
}

// A partition exchanges elements: either two stores to non-stack memory of
// values loaded from non-stack memory, or a call to a (swap) helper.  A graph
// DFS -- `vis[u] = 1; for (v...) if (adj[u][v] && !vis[v]) dfs(v);` -- has the
// loop, the compares and the recursion, but stores only a constant and calls
// only itself.
static bool exchangesElements(const ssa::SSAFunction& fn)
{
	for (const auto& blk: fn.blocks())
	{
		if (!blk) continue;
		for (const auto* i: blk->instrs)
			if (i && i->op == ssa::IrInstr::Op::Call && !i->calleeName.empty() && i->calleeName != fn.name())
				return true;
	}
	return ssa::countElementMoves(fn) >= 2;
}

} // anonymous namespace

SortResult QuicksortDetector::detect(const ssa::SSAFunction& fn) const
{
	SortResult result;
	result.algorithm = SortAlgorithm::Quicksort;

	if (isPartitionThenRecurse(fn))
	{
		result.confidence = 0.60f;
		result.compilerVariant = CompilerVariant::Unknown;
		return result;
	}

	PartitionFingerprint pf;
	auto ev = pf.analyse(fn);
	if (!ev.found || ev.confidence < 0.45f) return result;
	// A partition is a loop.  Heapify -- compares, a swap, one recursive
	// call, no loop -- matched the opcode bag and scored 1.0.
	if (!ssa::hasCycle(fn)) return result;

	// Introsort also has a partition phase; skip when depth-counter is present.
	if (hasDepthCounter(fn)) return result;

	// B8-loop: partition-shaped FIR/histogram/dot-product loops were
	// labelled quicksort at 0.90 with empirical precision 0. Require a
	// recursive self-call. Iterative quicksort is a miss (B9-style).
	if (countSelfCalls(fn) < 1) return result;
	if (!exchangesElements(fn)) return result;

	float score = ev.confidence + 0.25f;
	if (score > 1.0f) score = 1.0f;

	result.confidence = score;
	result.compilerVariant = CompilerVariant::Unknown;
	return result;
}

} // namespace sort_detect
} // namespace retdec
