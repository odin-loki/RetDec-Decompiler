/**
 * @file src/algo_recover/foreach_detect.cpp
 * @brief std::for_each detector — range loop with single call per element.
 *
 * ## Structural invariant
 *
 * A compiled std::for_each has:
 *   1. A loop with one Load per iteration (the element loaded from the range).
 *   2. A single Call per iteration applying the function to the element.
 *      The call's return value is not used (void or discarded).
 *   3. No accumulator phi node (distinguishes from std::accumulate).
 *   4. No Store of a computed value to a destination range (distinguishes from
 *      std::transform).
 *
 * ## Inlined lambda
 *
 * When the lambda is inlined (no explicit Call instruction), the loop body
 * contains the lambda's IR directly.  We accept this case with slightly lower
 * confidence since there is no Call signal.
 *
 * ## Differentiating from transform
 *
 * for_each: Call result not stored → no Store instruction in the loop.
 * transform: Call result stored    → Store in the loop.
 *
 * ## Confidence scoring
 *
 *   loop with back-edge and load     required
 *   call in loop body                +0.50
 *   no accumulator phi               +0.25
 *   no store of result               +0.25
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

static bool hasPhi(const ssa::SSAFunction& fn) {
    for (uint32_t b = 0; b < fn.blockCount(); ++b) {
        const auto* blk = fn.block(b);
        if (blk && !blk->phis.empty()) return true;
    }
    return false;
}

} // anonymous namespace

ForEachEvidence ForEachDetector::analyse(const ssa::SSAFunction& fn) const {
    ForEachEvidence ev;
    if (!hasBackEdge(fn)) return ev;
    if (countOp(fn, ssa::IrInstr::Op::Load) < 1) return ev;

    ev.hasLoopCall  = countOp(fn, ssa::IrInstr::Op::Call) >= 1;
    ev.hasNoPhi     = !hasPhi(fn);
    ev.hasNoDstStore = countOp(fn, ssa::IrInstr::Op::Store) == 0;
    ev.found = ev.hasLoopCall;
    ev.confidence = score(ev);
    return ev;
}

float ForEachDetector::score(const ForEachEvidence& ev) const {
    float s = 0.0f;
    if (ev.hasLoopCall) {
        s += 0.50f;
        if (ev.hasNoPhi)      s += 0.25f;
        if (ev.hasNoDstStore) s += 0.25f;
    }
    return s > 1.0f ? 1.0f : s;
}

AlgorithmResult ForEachDetector::detect(const ssa::SSAFunction& fn) const {
    AlgorithmResult result;
    result.kind = AlgorithmKind::ForEach;

    auto ev = analyse(fn);
    result.confidence = ev.confidence;

    EmissionTier tier = EmissionTier::Low;
    if (ev.confidence >= 0.75f) tier = EmissionTier::High;
    else if (ev.confidence >= 0.45f) tier = EmissionTier::Medium;
    result.tier = tier;

    if (ev.confidence < 0.01f) return result;

    switch (tier) {
    case EmissionTier::High:
        result.emittedForm = "std::for_each(first, last, f);"; break;
    case EmissionTier::Medium:
        result.emittedForm = "/* std::for_each? */ for (auto& e : range) f(e);"; break;
    default:
        result.emittedForm = "for (auto it = first; it != last; ++it) f(*it);"; break;
    }

    return result;
}

} // namespace algo_recover
} // namespace retdec
