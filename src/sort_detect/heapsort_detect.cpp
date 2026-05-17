/**
 * @file src/sort_detect/heapsort_detect.cpp
 * @brief Heapsort detector implementation.
 *
 * Heapsort has two structurally distinct phases:
 *
 * ### Build-heap phase
 *
 * A downward-counting loop from n/2 down to 0 that calls sift-down for each
 * position.  In IR:
 *   - A loop with a decrementing induction variable (Sub).
 *   - A Compare + CondBranch for the termination condition.
 *   - A call to sift-down (which may be inlined or a separate function).
 *
 * ### Sort phase
 *
 * A downward-counting loop from n-1 down to 1 that:
 *   1. Swaps the root (maximum) with the last element.
 *   2. Calls sift-down on the reduced heap.
 *
 * In IR this is another loop (separate from build-heap) containing:
 *   - A Store pair (swap) near the root/last positions.
 *   - A Sub decrement of the loop counter.
 *   - A call/inline of sift-down.
 *
 * ### Sift-down signature (from SiftDownFingerprint)
 *
 *   - Child index arithmetic: Shl(i, 1) + 1  (≡ 2*i+1)  for left child.
 *   - Mul(i, 2) + 2  for right child.
 *   - Max-child selection: Compare + CondBranch.
 *   - Conditional swap if parent < max-child.
 *   - Tail-recursive or iterative loop down the tree.
 *
 * Confidence:
 *   build-heap phase detected              +0.30
 *   sift-down signature                    +0.40
 *   sort phase (swap + sift-down again)    +0.30
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

// Build-heap phase: a loop with Sub (decrement) + Compare + CondBranch.
// Also, the loop calls something (sift-down) or contains sift-down inline.
static bool hasBuildHeapPhase(const ssa::SSAFunction& fn) {
    bool hasSub  = countOp(fn, ssa::IrInstr::Op::Sub) >= 1;
    bool hasCmp  = countOp(fn, ssa::IrInstr::Op::Compare) >= 1;
    bool hasCb   = countOp(fn, ssa::IrInstr::Op::CondBranch) >= 1;
    return hasSub && hasCmp && hasCb;
}

// Sort phase: an additional loop with a swap (2+ stores) + sift-down call/inline.
// Heuristic: the function has 2+ stores AND 2+ Subs (one for each phase).
static bool hasSortPhase(const ssa::SSAFunction& fn) {
    int stores = countOp(fn, ssa::IrInstr::Op::Store);
    int subs   = countOp(fn, ssa::IrInstr::Op::Sub);
    return stores >= 2 && subs >= 2;
}

} // anonymous namespace

// ─── HeapsortDetector ─────────────────────────────────────────────────────────

bool HeapsortDetector::hasBuildHeapPhase(const ssa::SSAFunction& fn) const {
    return ::retdec::sort_detect::hasBuildHeapPhase(fn);
}

bool HeapsortDetector::hasSortPhase(const ssa::SSAFunction& fn) const {
    return ::retdec::sort_detect::hasSortPhase(fn);
}

SortResult HeapsortDetector::detect(const ssa::SSAFunction& fn) const {
    SortResult result;
    result.algorithm = SortAlgorithm::Heapsort;

    float score = 0.0f;

    SiftDownFingerprint sdf;
    auto sde = sdf.analyse(fn);
    if (sde.found) score += 0.40f;

    if (hasBuildHeapPhase(fn)) score += 0.30f;
    if (hasSortPhase(fn))      score += 0.30f;

    result.confidence = score > 1.0f ? 1.0f : score;

    // Variant detection from function / callee names.
    const std::string& name = fn.name();
    if (name.find("sort_heap")  != std::string::npos ||
        name.find("make_heap")  != std::string::npos ||
        name.find("push_heap")  != std::string::npos ||
        name.find("sift_down")  != std::string::npos ||
        name.find("__sift")     != std::string::npos)
        result.compilerVariant = CompilerVariant::GCC;
    else if (name.find("_Push_heap") != std::string::npos ||
             name.find("_Pop_heap")  != std::string::npos)
        result.compilerVariant = CompilerVariant::MSVC;

    // Record sift-down as a helper if we detected it in callee names.
    for (uint32_t b = 0; b < fn.blockCount(); ++b) {
        const auto* blk = fn.block(b);
        if (!blk) continue;
        for (const auto* instr : blk->instrs) {
            if (!instr || instr->op != ssa::IrInstr::Op::Call) continue;
            const std::string& cn = instr->calleeName;
            if (cn.find("sift") != std::string::npos ||
                cn.find("heap") != std::string::npos)
                result.helperFunctions.push_back(cn);
        }
    }

    return result;
}

} // namespace sort_detect
} // namespace retdec
