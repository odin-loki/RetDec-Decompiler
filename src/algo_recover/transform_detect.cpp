/**
 * @file src/algo_recover/transform_detect.cpp
 * @brief std::transform detector — source→destination one-to-one loop.
 *
 * ## Structural invariant
 *
 * A compiled std::transform (or its manual equivalent) has:
 *
 *   1. A loop with one Load from a source iterator and one Store to a
 *      destination iterator, both incremented by the same stride each iteration.
 *   2. The value stored is derived from the value loaded (one-to-one mapping).
 *   3. The source and destination pointers have different base values (otherwise
 *      this is an in-place map, which we still accept but note).
 *   4. An inlined lambda / function call appears between the Load and Store when
 *      the transform is not identity (std::copy).
 *
 * ## Differentiating from std::copy
 *
 * std::copy is transform with identity function: stored value == loaded value.
 * We emit it as `std::copy` when there is no intervening computation.
 *
 * ## Back-inserter pattern
 *
 * When the destination is not a random-access range but a container's
 * push_back (output via `std::back_inserter`), the Store is replaced by a
 * Call to `push_back` / `_M_insert` / `emplace_back`.
 *
 * ## Confidence scoring
 *
 *   Loop with back-edge              required
 *   Load in loop body                +0.25
 *   Store in loop body               +0.25
 *   Both src and dst ptrs advanced   +0.25
 *   Derived store value (not raw)    +0.15 (lambda/call)
 *   Back-inserter call               +0.10 (replaces Store signal)
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
        for (uint32_t s : blk->succs)
            if (s <= b) return true;
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

// Both source and destination pointers advanced by Add in loop body.
static bool hasTwoPtrsAdvanced(const ssa::SSAFunction& fn) {
    return countOp(fn, ssa::IrInstr::Op::Add) >= 2;
}

// Derived store: a Call or any non-trivial computation between Load and Store.
// Note: 2 Adds are expected for src/dst pointer advances; require 3+ for computation.
static bool hasDerivedStore(const ssa::SSAFunction& fn) {
    return countOp(fn, ssa::IrInstr::Op::Call) >= 1 ||
           countOp(fn, ssa::IrInstr::Op::Add)  >= 3 ||
           countOp(fn, ssa::IrInstr::Op::Mul)  >= 1 ||
           countOp(fn, ssa::IrInstr::Op::Shl)  >= 1;
}

// Back-inserter: push_back / emplace_back / _M_insert call in loop.
static bool hasBackInserterCall(const ssa::SSAFunction& fn) {
    for (uint32_t b = 0; b < fn.blockCount(); ++b) {
        const auto* blk = fn.block(b);
        if (!blk) continue;
        for (const auto* i : blk->instrs) {
            if (!i || i->op != ssa::IrInstr::Op::Call) continue;
            const auto& cn = i->calleeName;
            if (cn.find("push_back")    != std::string::npos ||
                cn.find("emplace_back") != std::string::npos ||
                cn.find("_M_insert")    != std::string::npos ||
                cn.find("append")       != std::string::npos)
                return true;
        }
    }
    return false;
}

} // anonymous namespace

TransformEvidence TransformDetector::analyse(const ssa::SSAFunction& fn) const {
    TransformEvidence ev;
    if (!hasBackEdge(fn)) return ev;

    ev.hasSrcDstLoad      = countOp(fn, ssa::IrInstr::Op::Load)  >= 1 &&
                            countOp(fn, ssa::IrInstr::Op::Store) >= 1;
    ev.hasTwoPtrsAdvanced = hasTwoPtrsAdvanced(fn);
    ev.hasNoReorder       = countOp(fn, ssa::IrInstr::Op::Call)  <= 1; // no sort call
    ev.hasLambdaCall      = hasDerivedStore(fn);
    ev.hasBackInserter    = hasBackInserterCall(fn);
    ev.found = ev.hasSrcDstLoad;
    ev.confidence = score(ev);
    return ev;
}

float TransformDetector::score(const TransformEvidence& ev) const {
    if (!ev.hasSrcDstLoad) return 0.0f;
    float s = 0.0f;
    if (ev.hasSrcDstLoad)      s += 0.25f;
    if (ev.hasTwoPtrsAdvanced) s += 0.25f;
    if (ev.hasNoReorder)       s += 0.10f;
    if (ev.hasLambdaCall)      s += 0.30f;
    if (ev.hasBackInserter)    s += 0.10f;
    return s > 1.0f ? 1.0f : s;
}

std::string TransformDetector::emit(const TransformEvidence& ev,
                                     EmissionTier tier) const {
    if (tier == EmissionTier::Low)
        return "for (auto src = first; src != last; ++src, ++dst) *dst = f(*src);";

    // Determine whether identity (copy) or true transform.
    bool isIdentity = !ev.hasLambdaCall && !ev.hasBackInserter;

    if (tier == EmissionTier::Medium) {
        if (isIdentity)
            return "/* std::copy? */ for (...) *dst++ = *src++;";
        return "/* std::transform? */ for (...) *dst++ = f(*src++);";
    }

    // High tier.
    if (isIdentity)
        return ev.hasBackInserter
               ? "std::copy(first, last, std::back_inserter(dst));"
               : "std::copy(first, last, dst);";
    return ev.hasBackInserter
           ? "std::transform(first, last, std::back_inserter(dst), f);"
           : "std::transform(first, last, dst, f);";
}

AlgorithmResult TransformDetector::detect(const ssa::SSAFunction& fn) const {
    AlgorithmResult result;
    result.kind = AlgorithmKind::Transform;

    auto ev = analyse(fn);
    result.confidence = ev.confidence;
    result.hasLambda  = ev.hasLambdaCall;
    result.hasBackInserter = ev.hasBackInserter;

    // Assign tier (caller will override; set tentative here).
    EmissionTier tier = EmissionTier::Low;
    if (ev.confidence >= 0.75f) tier = EmissionTier::High;
    else if (ev.confidence >= 0.45f) tier = EmissionTier::Medium;
    result.tier = tier;

    if (ev.confidence < 0.01f) return result;

    // Identity transform → emit as copy.
    if (!ev.hasLambdaCall && !ev.hasBackInserter)
        result.kind = AlgorithmKind::Copy;

    result.emittedForm = emit(ev, tier);
    return result;
}

} // namespace algo_recover
} // namespace retdec
