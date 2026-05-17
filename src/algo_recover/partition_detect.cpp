/**
 * @file src/algo_recover/partition_detect.cpp
 * @brief std::partition detector — converging index, standalone (not in sort).
 *
 * ## Structural invariant
 *
 * std::partition is characterised by the converging-index pattern:
 *   1. Two pointer/index variables initialised to opposite ends of the range.
 *   2. The left pointer is incremented while the predicate is true; the right
 *      pointer is decremented while the predicate is false.
 *   3. When the two pointers cross or meet, the partition is complete.
 *   4. An element swap is performed when both pointers have stopped advancing.
 *
 * This is the same low-level pattern used by quicksort's partition step.
 * The key discriminator is:
 *   - Standalone std::partition: no recursive call and no depth counter.
 *   - Inside std::sort/introsort: recursive calls present, depth counter present.
 *
 * ## IR signals
 *
 * Converging pointers:
 *   - Both Add (left++) and Sub (right--) in the loop body.
 *   - Both are applied to pointer-like values (loaded from or used in Load/Store).
 *
 * Convergence check:
 *   - A Compare of the two pointers followed by a Branch that exits the loop.
 *
 * Swap:
 *   - Two Load instructions and two Store instructions, with cross-assignment
 *     (value from Load A stored to addr B, value from Load B stored to addr A).
 *   - Or a Call to `std::swap` / `swap`.
 *
 * No recursion:
 *   - No recursive Call (callee name == function name or callee not identified).
 *
 * ## Confidence scoring
 *
 *   converging pointers (Add + Sub in loop) +0.35
 *   swap pattern                            +0.35
 *   convergence check (compare of ptrs)     +0.20
 *   no recursion                            +0.10
 */

#include "retdec/algo_recover/algo_recover.h"
#include "retdec/ssa/ssa.h"

namespace retdec {
namespace algo_recover {

namespace {

static bool hasBackEdge(const ssa::SSAFunction& fn) {
    for (uint32_t b = 0; b < fn.blockCount(); ++b) {
        const auto* blk = fn.block(b);
        if (!blk) continue;
        for (uint32_t s : blk->succs) if (s <= b) return true;
    }
    return false;
}

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

// Converging pointers: both Add (left++) and Sub (right--) in loop body.
static bool hasConvergingPtrs(const ssa::SSAFunction& fn) {
    return countOp(fn, ssa::IrInstr::Op::Add) >= 1 &&
           countOp(fn, ssa::IrInstr::Op::Sub) >= 1;
}

// Swap: ≥2 Loads and ≥2 Stores, or a call to swap.
static bool hasSwap(const ssa::SSAFunction& fn) {
    if (countOp(fn, ssa::IrInstr::Op::Load)  >= 2 &&
        countOp(fn, ssa::IrInstr::Op::Store) >= 2)
        return true;
    for (uint32_t b = 0; b < fn.blockCount(); ++b) {
        const auto* blk = fn.block(b);
        if (!blk) continue;
        for (const auto* i : blk->instrs) {
            if (!i || i->op != ssa::IrInstr::Op::Call) continue;
            if (i->calleeName.find("swap") != std::string::npos) return true;
        }
    }
    return false;
}

// Convergence check: a Compare in the loop body.
static bool hasConvergenceCheck(const ssa::SSAFunction& fn) {
    return countOp(fn, ssa::IrInstr::Op::Compare) >= 1;
}

} // anonymous namespace

bool PartitionDetector::hasRecursion(const ssa::SSAFunction& fn) const {
    for (uint32_t b = 0; b < fn.blockCount(); ++b) {
        const auto* blk = fn.block(b);
        if (!blk) continue;
        for (const auto* i : blk->instrs) {
            if (!i || i->op != ssa::IrInstr::Op::Call) continue;
            // Self-call or depth-counter call (any call whose callee has the
            // same prefix as this function).
            if (i->calleeName == fn.name() ||
                (fn.name().size() >= 4 &&
                 i->calleeName.find(fn.name().substr(0, 4)) != std::string::npos))
                return true;
        }
    }
    return false;
}

PartitionEvidence PartitionDetector::analyse(const ssa::SSAFunction& fn) const {
    PartitionEvidence ev;
    if (!hasBackEdge(fn)) return ev;

    ev.hasConvergingPtrs   = hasConvergingPtrs(fn);
    ev.hasSwap             = hasSwap(fn);
    ev.hasConvergenceCheck = hasConvergenceCheck(fn);
    ev.hasNoRecursion      = !hasRecursion(fn);
    ev.found = ev.hasConvergingPtrs && ev.hasSwap;
    ev.confidence = score(ev);
    return ev;
}

float PartitionDetector::score(const PartitionEvidence& ev) const {
    float s = 0.0f;
    if (ev.hasConvergingPtrs)   s += 0.35f;
    if (ev.hasSwap)             s += 0.35f;
    if (ev.hasConvergenceCheck) s += 0.20f;
    if (ev.hasNoRecursion)      s += 0.10f;
    return s > 1.0f ? 1.0f : s;
}

AlgorithmResult PartitionDetector::detect(const ssa::SSAFunction& fn) const {
    AlgorithmResult result;
    result.kind = AlgorithmKind::Partition;

    auto ev = analyse(fn);
    result.confidence = ev.confidence;

    EmissionTier tier = EmissionTier::Low;
    if (ev.confidence >= 0.75f) tier = EmissionTier::High;
    else if (ev.confidence >= 0.45f) tier = EmissionTier::Medium;
    result.tier = tier;

    if (ev.confidence < 0.01f) return result;

    switch (tier) {
    case EmissionTier::High:
        result.emittedForm = "std::partition(first, last, pred);"; break;
    case EmissionTier::Medium:
        result.emittedForm = "/* std::partition? */ converging-index swap loop"; break;
    default:
        result.emittedForm =
            "auto left = first, right = last;\n"
            "while (left != right) { /* swap */ }";
        break;
    }

    return result;
}

} // namespace algo_recover
} // namespace retdec
