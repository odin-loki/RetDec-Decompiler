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

namespace retdec {
namespace sort_detect {

namespace {

static int countOp(const ssa::SSAFunction& fn, ssa::IrInstr::Op op) {
    int n = 0;
    for (uint32_t b = 0; b < fn.blockCount(); ++b) {
        const auto* blk = fn.block(b);
        if (!blk) continue;
        for (const auto* instr : blk->instrs)
            if (instr && instr->op == op) ++n;
    }
    return n;
}

static bool hasSwapInBlock(const ssa::BasicBlock& blk) {
    int stores = 0;
    for (const auto* instr : blk.instrs) {
        if (instr && instr->op == ssa::IrInstr::Op::Store) ++stores;
    }
    return stores >= 2;
}

static bool hasSwapPattern(const ssa::SSAFunction& fn) {
    for (uint32_t b = 0; b < fn.blockCount(); ++b) {
        const auto* blk = fn.block(b);
        if (blk && hasSwapInBlock(*blk)) return true;
    }
    return false;
}

static int countPhis(const ssa::SSAFunction& fn) {
    int n = 0;
    for (const auto& phi : fn.phis())
        if (phi) ++n;
    return n;
}

static int countSelfCalls(const ssa::SSAFunction& fn) {
    int n = 0;
    const std::string& name = fn.name();
    for (uint32_t b = 0; b < fn.blockCount(); ++b) {
        const auto* blk = fn.block(b);
        if (!blk) continue;
        for (const auto* instr : blk->instrs)
            if (instr && instr->op == ssa::IrInstr::Op::Call &&
                instr->calleeName == name)
                ++n;
    }
    return n;
}

} // anonymous namespace

SortResult BubbleSortDetector::detect(const ssa::SSAFunction& fn) const {
    SortResult result;
    result.algorithm = SortAlgorithm::BubbleSort;

    PartitionFingerprint pf;
    const auto part = pf.analyse(fn);
    if (part.found && part.confidence >= 0.45f)
        return result;

    SiftDownFingerprint sdf;
    if (sdf.analyse(fn).found)
        return result;

    const int cmps  = countOp(fn, ssa::IrInstr::Op::Compare);
    const int cb    = countOp(fn, ssa::IrInstr::Op::CondBranch);
    const int phis  = countPhis(fn);
    const bool swap = hasSwapPattern(fn);
    const int self  = countSelfCalls(fn);

    if (cmps < 3 || !swap || self > 0 || cb < 3 || phis < 2)
        return result;

    float score = 0.0f;
    if (cmps >= 3)           score += 0.30f;
    if (swap)                score += 0.25f;
    if (cb >= 3)             score += 0.25f;
    if (phis >= 2)           score += 0.20f;

    result.confidence = score > 1.0f ? 1.0f : score;
    result.compilerVariant = CompilerVariant::Unknown;
    return result;
}

} // namespace sort_detect
} // namespace retdec
