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

// A Hoare partition's two indices *converge*: the left one is advanced by an
// Add that feeds its loop-header phi, the right one is retreated by a Sub that
// feeds its own phi.  Bubble sort also contains an Add and a Sub, but its Sub
// computes the inner-loop *bound* (n - 1 - i) and never flows back into a phi,
// so a loop-carried decrement is the one partition signal a bubble sort does
// not produce.  Everything else PartitionFingerprint scores — an element
// compare, a two-store swap, a couple of conditional branches — is shared with
// bubble sort, which is why the score alone cannot separate the two.
static bool hasConvergingIndexPhis(const ssa::SSAFunction& fn) {
    bool advancing = false;
    bool retreating = false;
    for (const auto& phi : fn.phis()) {
        if (!phi) continue;
        for (const auto& op : phi->operands) {
            const auto* val = fn.value(op.second);
            if (!val || !val->defInstr) continue;
            if (val->defInstr->op == ssa::IrInstr::Op::Add) advancing = true;
            if (val->defInstr->op == ssa::IrInstr::Op::Sub) retreating = true;
        }
    }
    return advancing && retreating;
}

} // anonymous namespace

SortResult BubbleSortDetector::detect(const ssa::SSAFunction& fn) const {
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
    if (part.found && part.confidence >= 0.45f && hasConvergingIndexPhis(fn))
        return result;

    // Same mistake, second guard: SiftDownEvidence.found needs two of its
    // three signals, and two of them — a compare feeding a conditional branch,
    // and a two-store swap — are exactly what a bubble sort has.  Only the
    // child-index arithmetic (2*i+1, i.e. Shl by 1 / Mul by 2) is heap-specific,
    // so require it before letting sift-down evidence veto a bubble sort.
    SiftDownFingerprint sdf;
    const auto sift = sdf.analyse(fn);
    if (sift.found && sift.hasLeftArith)
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
