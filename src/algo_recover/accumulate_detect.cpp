/**
 * @file src/algo_recover/accumulate_detect.cpp
 * @brief std::accumulate / max_element / min_element detector.
 *
 * ## Structural invariant
 *
 * A compiled std::accumulate has:
 *   1. A phi node carrying the accumulator value across loop iterations.
 *   2. A binary operation combining the accumulator with the element loaded
 *      from the range (Add, Mul, Or, Xor, And for accumulate; Compare+Select
 *      for max/min variants).
 *   3. No Store inside the loop body (the accumulator exists only in registers
 *      or the phi value stream — not memory).
 *
 * ## Combiner detection
 *
 * The specific binary operation used determines which standard algorithm to emit:
 *
 * | IR opcode   | Combiner     | Emitted form                                    |
 * |-------------|--------------|------------------------------------------------|
 * | Add         | Add          | std::accumulate(first, last, 0)                 |
 * | Mul         | Mul          | std::accumulate(first, last, 1, std::multiplies<>{}) |
 * | Or          | Or           | std::accumulate(first, last, 0, std::bit_or<>{}) |
 * | Xor         | Xor          | std::accumulate(first, last, 0, std::bit_xor<>{}) |
 * | And         | And          | std::accumulate(first, last, ~0, std::bit_and<>{}) |
 * | Compare+max | Max          | *std::max_element(first, last)                  |
 * | Compare+min | Min          | *std::min_element(first, last)                  |
 *
 * ## Confidence scoring
 *
 *   phi node present                +0.40
 *   binary op combining phi+element +0.35
 *   no store in loop                +0.25
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

// Phi nodes: check via fn.blockCount() iterating blocks that have phis.
static bool hasPhi(const ssa::SSAFunction& fn) {
    for (uint32_t b = 0; b < fn.blockCount(); ++b) {
        const auto* blk = fn.block(b);
        if (blk && !blk->phis.empty()) return true;
    }
    return false;
}

} // anonymous namespace

CombinerKind AccumulateDetector::detectCombiner(const ssa::SSAFunction& fn) const {
    // Check for the Compare+select pattern first (max/min).
    if (countOp(fn, ssa::IrInstr::Op::Compare) >= 1) {
        // Max: greater-than compare, result used to select max.
        // Min: less-than compare.
        // We can't distinguish without condition flags; return Max as heuristic
        // when there is a compare but no explicit add/mul/or/xor.
        if (countOp(fn, ssa::IrInstr::Op::Add) == 0 &&
            countOp(fn, ssa::IrInstr::Op::Mul) == 0 &&
            countOp(fn, ssa::IrInstr::Op::Or)  == 0 &&
            countOp(fn, ssa::IrInstr::Op::Xor) == 0)
            return CombinerKind::Max;  // could be Min too; caller refines
    }
    if (countOp(fn, ssa::IrInstr::Op::Mul) >= 1) return CombinerKind::Mul;
    if (countOp(fn, ssa::IrInstr::Op::Or)  >= 1) return CombinerKind::Or;
    if (countOp(fn, ssa::IrInstr::Op::Xor) >= 1) return CombinerKind::Xor;
    if (countOp(fn, ssa::IrInstr::Op::And) >= 1) return CombinerKind::And;
    if (countOp(fn, ssa::IrInstr::Op::Add) >= 1) return CombinerKind::Add;
    return CombinerKind::Unknown;
}

AccumulateEvidence AccumulateDetector::analyse(const ssa::SSAFunction& fn) const {
    AccumulateEvidence ev;
    if (!hasBackEdge(fn)) return ev;

    ev.hasPhi     = hasPhi(fn);
    ev.hasBinOp   = countOp(fn, ssa::IrInstr::Op::Add) >= 1 ||
                    countOp(fn, ssa::IrInstr::Op::Mul) >= 1 ||
                    countOp(fn, ssa::IrInstr::Op::Or)  >= 1 ||
                    countOp(fn, ssa::IrInstr::Op::Xor) >= 1 ||
                    countOp(fn, ssa::IrInstr::Op::And) >= 1 ||
                    countOp(fn, ssa::IrInstr::Op::Compare) >= 1;
    ev.hasNoStore = countOp(fn, ssa::IrInstr::Op::Store) == 0;
    ev.combiner   = detectCombiner(fn);
    ev.found = ev.hasPhi && ev.hasBinOp;
    ev.confidence = score(ev);
    return ev;
}

float AccumulateDetector::score(const AccumulateEvidence& ev) const {
    float s = 0.0f;
    if (ev.hasPhi)     s += 0.40f;
    if (ev.hasBinOp)   s += 0.35f;
    if (ev.hasNoStore) s += 0.25f;
    return s > 1.0f ? 1.0f : s;
}

std::string AccumulateDetector::emit(const AccumulateEvidence& ev,
                                      AlgorithmKind k,
                                      EmissionTier tier) const {
    if (tier == EmissionTier::Low)
        return "T acc = init; for (auto it = first; it != last; ++it) acc = acc op *it;";

    if (k == AlgorithmKind::MaxElement) {
        if (tier == EmissionTier::Medium) return "/* std::max_element? */ max loop";
        return "*std::max_element(first, last);";
    }
    if (k == AlgorithmKind::MinElement) {
        if (tier == EmissionTier::Medium) return "/* std::min_element? */ min loop";
        return "*std::min_element(first, last);";
    }

    if (tier == EmissionTier::Medium) return "/* std::accumulate? */ acc loop";

    switch (ev.combiner) {
    case CombinerKind::Add: return "std::accumulate(first, last, 0);";
    case CombinerKind::Mul: return "std::accumulate(first, last, 1, std::multiplies<>{});";
    case CombinerKind::Or:  return "std::accumulate(first, last, 0, std::bit_or<>{});";
    case CombinerKind::Xor: return "std::accumulate(first, last, 0, std::bit_xor<>{});";
    case CombinerKind::And: return "std::accumulate(first, last, ~0, std::bit_and<>{});";
    default:                return "std::accumulate(first, last, init, op);";
    }
}

AlgorithmResult AccumulateDetector::detect(const ssa::SSAFunction& fn) const {
    AlgorithmResult result;
    result.kind = AlgorithmKind::Accumulate;

    auto ev = analyse(fn);
    result.confidence = ev.confidence;
    result.combiner   = ev.combiner;

    EmissionTier tier = EmissionTier::Low;
    if (ev.confidence >= 0.75f) tier = EmissionTier::High;
    else if (ev.confidence >= 0.45f) tier = EmissionTier::Medium;
    result.tier = tier;

    if (ev.confidence < 0.01f) return result;

    // Promote to max/min_element when combiner is Max/Min.
    AlgorithmKind kind = AlgorithmKind::Accumulate;
    if (ev.combiner == CombinerKind::Max) kind = AlgorithmKind::MaxElement;
    if (ev.combiner == CombinerKind::Min) kind = AlgorithmKind::MinElement;
    result.kind = kind;

    result.emittedForm = emit(ev, kind, tier);
    return result;
}

} // namespace algo_recover
} // namespace retdec
