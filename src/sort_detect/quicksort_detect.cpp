/**
 * @file src/sort_detect/quicksort_detect.cpp
 * @brief Quicksort detector — partition loop without introsort depth counter.
 */

#include "retdec/sort_detect/sort_detect.h"
#include "retdec/ssa/ssa.h"

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

} // anonymous namespace

SortResult QuicksortDetector::detect(const ssa::SSAFunction& fn) const
{
	SortResult result;
	result.algorithm = SortAlgorithm::Quicksort;

	PartitionFingerprint pf;
	auto ev = pf.analyse(fn);
	if (!ev.found || ev.confidence < 0.45f) return result;

	// Introsort also has a partition phase; skip when depth-counter is present.
	if (hasDepthCounter(fn)) return result;

	// B8-loop: partition-shaped FIR/histogram/dot-product loops were
	// labelled quicksort at 0.90 with empirical precision 0. Require a
	// recursive self-call. Iterative quicksort is a miss (B9-style).
	if (countSelfCalls(fn) < 1) return result;

	float score = ev.confidence + 0.25f;
	if (score > 1.0f) score = 1.0f;

	result.confidence = score;
	result.compilerVariant = CompilerVariant::Unknown;
	return result;
}

} // namespace sort_detect
} // namespace retdec
