/**
 * @file src/algo_recover/binary_search_detect.cpp
 * @brief Binary search detector — halving midpoint on sorted range.
 */

#include "retdec/algo_recover/algo_recover.h"
#include "retdec/ssa/ssa.h"

namespace retdec {
namespace algo_recover {

namespace {

static int countOp(const ssa::SSAFunction& fn, ssa::IrInstr::Op op) {
    int n = 0;
    for (uint32_t b = 0; b < fn.blockCount(); ++b) {
        const auto* blk = fn.block(b);
        if (!blk) continue;
        for (const auto* i : blk->instrs)
            if (i && i->op == op) ++n;
    }
    return n;
}

static bool hasBackEdge(const ssa::SSAFunction& fn) {
    for (uint32_t b = 0; b < fn.blockCount(); ++b) {
        const auto* blk = fn.block(b);
        if (!blk) continue;
        for (uint32_t s : blk->succs)
            if (s <= b) return true;
    }
    return false;
}

} // anonymous namespace

AlgorithmResult BinarySearchDetector::detect(const ssa::SSAFunction& fn) const {
    AlgorithmResult result;
    result.kind = AlgorithmKind::BinarySearch;

    if (!hasBackEdge(fn))
        return result;

    const int cmps  = countOp(fn, ssa::IrInstr::Op::Compare);
    const int adds  = countOp(fn, ssa::IrInstr::Op::Add);
    const int subs  = countOp(fn, ssa::IrInstr::Op::Sub);
    const int loads = countOp(fn, ssa::IrInstr::Op::Load);
    const int stores = countOp(fn, ssa::IrInstr::Op::Store);
    const bool halving = countOp(fn, ssa::IrInstr::Op::Shr) >= 1
                      || countOp(fn, ssa::IrInstr::Op::Div) >= 1;

    if (cmps < 2 || loads < 1 || !halving)
        return result;

    float score = 0.0f;
    if (cmps >= 2)   score += 0.40f;
    if (halving)     score += 0.35f;
    if (adds >= 1 && subs >= 1) score += 0.25f;
    if (stores == 0) score += 0.10f;

    result.confidence = score > 1.0f ? 1.0f : score;
    if (result.confidence >= 0.75f)
        result.tier = EmissionTier::High;
    else if (result.confidence >= 0.45f)
        result.tier = EmissionTier::Medium;
    else
        result.tier = EmissionTier::Low;

    result.emittedForm = "binary_search(sorted, target)";
    return result;
}

} // namespace algo_recover
} // namespace retdec
