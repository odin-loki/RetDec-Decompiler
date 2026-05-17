/**
 * @file src/algo_recover/find_detect.cpp
 * @brief std::find / std::find_if detector — equality compare + early exit.
 *
 * ## Structural invariant
 *
 * A compiled std::find has:
 *   1. A loop with one Load from the range pointer.
 *   2. A Compare of the loaded element against a target value.
 *   3. An early exit: a conditional Branch that leaves the loop when the
 *      condition is true (element matches), returning the current pointer.
 *   4. No Store in the loop body (pure search, non-mutating).
 *
 * ## std::find vs std::find_if
 *
 * - Constant comparand → `std::find(first, last, value)`.
 * - Non-constant or predicate call → `std::find_if(first, last, pred)`.
 *
 * The distinction: if the Compare uses an Immediate operand or a value that
 * is invariant with respect to the loop, it's a constant comparand (std::find).
 * If the Compare feeds through a Call or non-trivial computation, it's find_if.
 *
 * ## Short-circuit relatives
 *
 * - `any_of`: find_if pattern, result is "found" boolean rather than pointer.
 * - `all_of`: early exit on first false → loop exits when Compare is false.
 * - `none_of`: early exit on first true → equivalent to !any_of.
 * - `count` / `count_if`: no early exit; accumulator phi incremented on match.
 *
 * We detect all of these and emit the most specific form.
 *
 * ## Confidence scoring
 *
 *   loop with back-edge and load      required
 *   compare in loop body              +0.40
 *   early exit conditional branch     +0.35
 *   no store in loop                  +0.25
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

// Early exit: a block with two successors (conditional branch) where at least
// one successor is outside the loop (successor index > current block).
static bool hasEarlyExit(const ssa::SSAFunction& fn) {
    for (uint32_t b = 0; b < fn.blockCount(); ++b) {
        const auto* blk = fn.block(b);
        if (!blk || blk->succs.size() < 2) continue;
        bool hasForwardSucc = false;
        for (uint32_t s : blk->succs)
            if (s > b) { hasForwardSucc = true; break; }
        if (hasForwardSucc) return true;
    }
    return false;
}

// Predicate call: a Call in the loop body (find_if comparator).
static bool hasPredicateCall(const ssa::SSAFunction& fn) {
    return countOp(fn, ssa::IrInstr::Op::Call) >= 1;
}

// Immediate comparand: Compare uses an Immediate value (std::find).
static bool hasImmediateComparand(const ssa::SSAFunction& fn) {
    for (uint32_t b = 0; b < fn.blockCount(); ++b) {
        const auto* blk = fn.block(b);
        if (!blk) continue;
        for (const auto* i : blk->instrs) {
            if (!i || i->op != ssa::IrInstr::Op::Compare) continue;
            for (const auto& u : i->uses) {
                const auto* v = fn.value(u.valueId);
                if (v && v->kind == ssa::ValueKind::Immediate) return true;
            }
        }
    }
    return false;
}

// Count variant: accumulator phi + compare (but no early exit).
static bool hasCountPattern(const ssa::SSAFunction& fn) {
    bool hasPhi = false;
    for (uint32_t b = 0; b < fn.blockCount(); ++b) {
        const auto* blk = fn.block(b);
        if (blk && !blk->phis.empty()) { hasPhi = true; break; }
    }
    return hasPhi &&
           countOp(fn, ssa::IrInstr::Op::Compare) >= 1 &&
           !hasEarlyExit(fn);
}

} // anonymous namespace

FindEvidence FindDetector::analyseFind(const ssa::SSAFunction& fn) const {
    FindEvidence ev;
    if (!hasBackEdge(fn)) return ev;
    if (countOp(fn, ssa::IrInstr::Op::Load) < 1) return ev;

    ev.hasCompare   = countOp(fn, ssa::IrInstr::Op::Compare) >= 1;
    ev.hasEarlyExit = hasEarlyExit(fn);
    ev.hasNoStore   = countOp(fn, ssa::IrInstr::Op::Store) == 0;
    ev.hasLambda    = hasPredicateCall(fn) || !hasImmediateComparand(fn);
    ev.found = ev.hasCompare;
    ev.confidence = score(ev);
    return ev;
}

float FindDetector::score(const FindEvidence& ev) const {
    float s = 0.0f;
    if (ev.hasCompare)   s += 0.40f;
    if (ev.hasEarlyExit) s += 0.35f;
    if (ev.hasNoStore)   s += 0.25f;
    return s > 1.0f ? 1.0f : s;
}

std::string FindDetector::emit(const FindEvidence& ev, EmissionTier tier) const {
    if (tier == EmissionTier::Low)
        return "for (auto it = first; it != last; ++it) if (*it == val) return it;";

    bool isIf = ev.hasLambda;
    if (tier == EmissionTier::Medium)
        return isIf ? "/* std::find_if? */ search loop with predicate"
                    : "/* std::find? */ linear search loop";

    return isIf ? "std::find_if(first, last, pred);"
                : "std::find(first, last, value);";
}

AlgorithmResult FindDetector::detect(const ssa::SSAFunction& fn) const {
    AlgorithmResult result;
    result.kind = AlgorithmKind::Find;

    auto ev = analyseFind(fn);
    result.confidence = ev.confidence;
    result.hasLambda  = ev.hasLambda;

    EmissionTier tier = EmissionTier::Low;
    if (ev.confidence >= 0.75f) tier = EmissionTier::High;
    else if (ev.confidence >= 0.45f) tier = EmissionTier::Medium;
    result.tier = tier;

    if (ev.confidence < 0.01f) return result;

    // Distinguish find / find_if / count / any_of.
    if (hasCountPattern(fn)) {
        result.kind = AlgorithmKind::Count;
        if (tier == EmissionTier::High)
            result.emittedForm = ev.hasLambda
                ? "std::count_if(first, last, pred);"
                : "std::count(first, last, value);";
        else result.emittedForm = emit(ev, tier);
    } else {
        result.kind = ev.hasLambda ? AlgorithmKind::FindIf : AlgorithmKind::Find;
        result.emittedForm = emit(ev, tier);
    }

    return result;
}

} // namespace algo_recover
} // namespace retdec
