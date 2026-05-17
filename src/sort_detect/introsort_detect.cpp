/**
 * @file src/sort_detect/introsort_detect.cpp
 * @brief Introsort detector implementation.
 *
 * Introsort (Musser 1997) is used by GCC's std::sort and MSVC's std::sort.
 * It combines quicksort, heapsort, and insertion sort.
 *
 * Structural fingerprint:
 *
 *   1. Partition phase — Hoare-style partition loop (scored by PartitionFingerprint).
 *   2. Depth counter — a loop depth variable (starts at log2(n)*2) that is
 *      decremented each recursive call; when it reaches zero the algorithm
 *      delegates to heapsort.  In IR this appears as a decrement of a variable
 *      (typically passed as a parameter or computed from block count) and a
 *      Compare against zero followed by a CondBranch to a different function.
 *   3. Insertion sort tail — for sub-ranges smaller than ~16 elements.
 *
 * Confidence computation:
 *   partition score (0–0.5 normalised)    → multiplied by 0.50
 *   recursive calls on sub-ranges          → +0.30 if selfCallCount >= 2
 *   insertion sort evidence                → +0.20 if InsertionSortFingerprint found
 *
 * Compiler variant inference:
 *   - GCC libstdc++: helper function name contains "_introsort" or "__sort".
 *   - Clang libc++:  helper name contains "__sort" or "__introsort_loop".
 *   - MSVC STL:      helper name contains "_Sort_unchecked" or "?std@@sort".
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

// Count self-recursive calls.
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

// Depth counter heuristic: a function argument (phi at entry) that is
// decremented (Sub or Add with negative immediate) and compared against zero,
// then branches to a heapsort delegate.
static bool hasDepthCounter(const ssa::SSAFunction& fn) {
    // Proxy: function has a Sub somewhere (depth counter decrements by 1 or 2)
    // and a Compare against zero (checking depth == 0).
    bool hasSub = countOp(fn, ssa::IrInstr::Op::Sub) >= 1;
    bool hasCmpZero = false;

    for (uint32_t b = 0; b < fn.blockCount(); ++b) {
        const auto* blk = fn.block(b);
        if (!blk) continue;
        for (const auto* instr : blk->instrs) {
            if (!instr || instr->op != ssa::IrInstr::Op::Compare) continue;
            for (const auto& use : instr->uses) {
                const auto* val = fn.value(use.valueId);
                if (val && val->kind == ssa::ValueKind::Immediate && val->imm == 0) {
                    hasCmpZero = true; break;
                }
            }
            if (hasCmpZero) break;
        }
        if (hasCmpZero) break;
    }
    return hasSub && hasCmpZero;
}

// Heapsort delegate: a call to a function whose name suggests heapsort,
// or a block with sift-down arithmetic (Shl by 1, Mul by 2).
static bool hasHeapsortDelegate(const ssa::SSAFunction& fn) {
    for (uint32_t b = 0; b < fn.blockCount(); ++b) {
        const auto* blk = fn.block(b);
        if (!blk) continue;
        for (const auto* instr : blk->instrs) {
            if (!instr || instr->op != ssa::IrInstr::Op::Call) continue;
            const std::string& callee = instr->calleeName;
            // Recognise common heapsort helper names.
            if (callee.find("heap") != std::string::npos ||
                callee.find("Heap") != std::string::npos ||
                callee.find("push_heap") != std::string::npos ||
                callee.find("sort_heap") != std::string::npos ||
                callee.find("make_heap") != std::string::npos)
                return true;
        }
    }
    return false;
}

} // anonymous namespace

// ─── IntrosortDetector ────────────────────────────────────────────────────────

bool IntrosortDetector::hasDepthCounter(const ssa::SSAFunction& fn) const {
    return ::retdec::sort_detect::hasDepthCounter(fn);
}

bool IntrosortDetector::hasHeapsortDelegate(const ssa::SSAFunction& fn) const {
    return ::retdec::sort_detect::hasHeapsortDelegate(fn);
}

CompilerVariant IntrosortDetector::detectVariant(const ssa::SSAFunction& fn) const {
    const std::string& name = fn.name();
    if (name.find("_introsort")  != std::string::npos ||
        name.find("__sort")      != std::string::npos)
        return CompilerVariant::GCC;
    if (name.find("introsort_loop") != std::string::npos)
        return CompilerVariant::Clang;
    if (name.find("Sort_unchecked") != std::string::npos ||
        name.find("?std@@sort")     != std::string::npos)
        return CompilerVariant::MSVC;

    // Check callee names for variant clues.
    for (uint32_t b = 0; b < fn.blockCount(); ++b) {
        const auto* blk = fn.block(b);
        if (!blk) continue;
        for (const auto* instr : blk->instrs) {
            if (!instr || instr->op != ssa::IrInstr::Op::Call) continue;
            if (instr->calleeName.find("_introsort") != std::string::npos)
                return CompilerVariant::GCC;
            if (instr->calleeName.find("_Sort_unchecked") != std::string::npos)
                return CompilerVariant::MSVC;
        }
    }
    return CompilerVariant::Unknown;
}

SortResult IntrosortDetector::detect(const ssa::SSAFunction& fn) const {
    SortResult result;
    result.algorithm = SortAlgorithm::Introsort;

    // Phase 1: Partition fingerprint.
    PartitionFingerprint pf;
    auto pe = pf.analyse(fn);
    float partScore = pe.confidence * 0.50f;

    // Phase 2: Recursive calls on sub-ranges.
    RecursiveHalvingFingerprint rhf;
    auto re = rhf.analyse(fn);
    float recursionScore = (re.selfCallCount >= 2) ? 0.30f : 0.0f;

    // Phase 3: Insertion sort tail.
    InsertionSortFingerprint isf;
    auto ie = isf.analyse(fn);
    float insertionScore = ie.found ? 0.20f : 0.0f;

    result.confidence = partScore + recursionScore + insertionScore;

    // Extra bonus for depth counter + heapsort delegate.
    if (hasDepthCounter(fn))    result.confidence += 0.05f;
    if (hasHeapsortDelegate(fn)) result.confidence += 0.05f;
    if (result.confidence > 1.0f) result.confidence = 1.0f;

    result.compilerVariant = detectVariant(fn);
    return result;
}

} // namespace sort_detect
} // namespace retdec
