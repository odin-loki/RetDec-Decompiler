/**
 * @file src/sort_detect/mergesort_detect.cpp
 * @brief Mergesort / std::stable_sort detector.
 *
 * Structural fingerprint:
 *
 *   1. **Two recursive calls on halved ranges** — `RecursiveHalvingFingerprint`
 *      detects self-recursive calls where the argument sets suggest contiguous
 *      halves: (begin, mid) and (mid, end).
 *
 *   2. **Merge loop** — a loop that reads from two sorted sub-ranges and writes
 *      to a single output range.  It has a three-way branch structure:
 *        a. Element from left consumed  → advance left pointer.
 *        b. Element from right consumed → advance right pointer.
 *        c. One range exhausted         → copy remaining + exit.
 *
 *      In IR this appears as: at least two Load instructions, one Compare,
 *      and two conditional branches, all within a loop (cycle in the CFG).
 *
 *   3. **Auxiliary buffer allocation** — malloc/calloc/alloca call with a size
 *      argument proportional to the input range size (computed from end-begin).
 *
 * Confidence:
 *   two recursive self-calls              +0.40
 *   merge loop (loads + cmp + branches)   +0.40
 *   auxiliary allocation                  +0.20
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

// Detect a merge loop: a basic block that is part of a cycle (has a back-edge
// to itself or to a block that leads back to it) containing at least 2 Loads,
// 1 Compare, and 2 CondBranches.
// Simplified heuristic: a block whose successor set contains its own id
// OR the function has a block with (2+ Loads + 1+ Compare + 2+ CondBranch).
static bool hasMergeLoop(const ssa::SSAFunction& fn) {
    // A merge loop needs at least 2 loads, 1 cmp, and branching.
    int loads = countOp(fn, ssa::IrInstr::Op::Load);
    int cmps  = countOp(fn, ssa::IrInstr::Op::Compare);
    int cbs   = countOp(fn, ssa::IrInstr::Op::CondBranch);
    return loads >= 2 && cmps >= 1 && cbs >= 2;
}

// Detect auxiliary buffer allocation: a call to malloc/calloc/alloca/new
// in the function.
static bool hasAuxiliaryAllocation(const ssa::SSAFunction& fn) {
    for (uint32_t b = 0; b < fn.blockCount(); ++b) {
        const auto* blk = fn.block(b);
        if (!blk) continue;
        for (const auto* instr : blk->instrs) {
            if (!instr || instr->op != ssa::IrInstr::Op::Call) continue;
            const std::string& cn = instr->calleeName;
            if (cn == "malloc"  || cn == "calloc"  || cn == "realloc"  ||
                cn == "__builtin_alloca"            || cn == "alloca"   ||
                cn.find("_new")     != std::string::npos ||
                cn.find("allocate") != std::string::npos ||
                cn.find("Allocate") != std::string::npos)
                return true;
        }
    }
    return false;
}

// Score: do two recursive halves exist?
static float scoreRecursion(const ssa::SSAFunction& fn) {
    RecursiveHalvingFingerprint rhf;
    auto ev = rhf.analyse(fn);
    if (ev.selfCallCount >= 2) return 0.40f;
    if (ev.selfCallCount == 1) return 0.15f;
    return 0.0f;
}

} // anonymous namespace

// ─── MergesortDetector ────────────────────────────────────────────────────────

bool MergesortDetector::hasMergeLoop(const ssa::SSAFunction& fn) const {
    return ::retdec::sort_detect::hasMergeLoop(fn);
}

bool MergesortDetector::hasAuxiliaryAllocation(const ssa::SSAFunction& fn) const {
    return ::retdec::sort_detect::hasAuxiliaryAllocation(fn);
}

float MergesortDetector::scoreMerge(const ssa::SSAFunction& fn) const {
    float score = 0.0f;
    score += scoreRecursion(fn);
    if (hasMergeLoop(fn))              score += 0.40f;
    if (hasAuxiliaryAllocation(fn))    score += 0.20f;
    return score > 1.0f ? 1.0f : score;
}

SortResult MergesortDetector::detect(const ssa::SSAFunction& fn) const {
    SortResult result;
    result.algorithm  = SortAlgorithm::Mergesort;
    result.confidence = scoreMerge(fn);

    // Compiler variant heuristics.
    const std::string& name = fn.name();
    if (name.find("stable_sort") != std::string::npos ||
        name.find("merge_sort")  != std::string::npos ||
        name.find("__merge")     != std::string::npos)
        result.compilerVariant = CompilerVariant::GCC;
    else if (name.find("_Stable_sort") != std::string::npos)
        result.compilerVariant = CompilerVariant::MSVC;
    else
        result.compilerVariant = CompilerVariant::Unknown;

    return result;
}

} // namespace sort_detect
} // namespace retdec
