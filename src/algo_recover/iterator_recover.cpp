/**
 * @file src/algo_recover/iterator_recover.cpp
 * @brief Iterator pattern recovery — begin/end → range-based for.
 *
 * ## Patterns recognised
 *
 * ### begin/end pair
 *
 * A loop that:
 *   - Receives (or computes) a begin and an end pointer.
 *   - Increments the begin pointer each iteration.
 *   - Compares the incremented pointer to the end pointer (the termination
 *     condition).
 *
 * In IR:
 *   ```
 *   LOOP:
 *     elem = Load(it)
 *     use(elem)
 *     it   = it + stride
 *     cmp  = Compare(it, end)
 *     Branch LOOP | EXIT
 *   ```
 *
 * Emitted form: `for (auto& e : v)` (range-based for).
 *
 * ### Reverse iterator
 *
 * A loop that:
 *   - Decrements the pointer each iteration (Sub in the loop body rather than Add).
 *   - Compares the pointer to the begin pointer.
 *
 * Emitted form: `for (auto& e : std::ranges::reverse_view(v))`.
 *
 * ### back_inserter
 *
 * The loop body contains a `push_back` / `emplace_back` Call where the
 * argument is the loaded element.  The output "iterator" is a back_inserter.
 *
 * Emitted form (as output iterator for transform/copy):
 *   `std::back_inserter(dst)`
 *
 * ## Implementation
 *
 * Since we cannot query the container type here (that requires `container_detect`
 * context), we emit the generic form.  The calling pass (CodeGenPass) will
 * substitute the actual container name.
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

} // anonymous namespace

bool IteratorPatternRecovery::hasBeginEndPair(const ssa::SSAFunction& fn) const {
    // A begin/end pattern: Load + Add (increment) + Compare (against end) + back-edge.
    return hasBackEdge(fn) &&
           countOp(fn, ssa::IrInstr::Op::Load)    >= 1 &&
           countOp(fn, ssa::IrInstr::Op::Add)     >= 1 &&
           countOp(fn, ssa::IrInstr::Op::Compare) >= 1;
}

bool IteratorPatternRecovery::hasReverseIter(const ssa::SSAFunction& fn) const {
    // Reverse: Sub (decrement) instead of Add, still has Compare + Load.
    if (!hasBackEdge(fn)) return false;
    bool hasAdd = countOp(fn, ssa::IrInstr::Op::Add) >= 1;
    bool hasSub = countOp(fn, ssa::IrInstr::Op::Sub) >= 1;
    // Reverse iterator: Sub present but no paired Add for the iterator increment
    // (Add is only from base pointer arithmetic of element access, not iteration).
    // Heuristic: Sub > 0 AND Compare present AND Sub is the dominant loop-closing op.
    return hasSub && !hasAdd &&
           countOp(fn, ssa::IrInstr::Op::Load)    >= 1 &&
           countOp(fn, ssa::IrInstr::Op::Compare) >= 1;
}

bool IteratorPatternRecovery::hasBackInserter(const ssa::SSAFunction& fn) const {
    for (uint32_t b = 0; b < fn.blockCount(); ++b) {
        const auto* blk = fn.block(b);
        if (!blk) continue;
        for (const auto* i : blk->instrs) {
            if (!i || i->op != ssa::IrInstr::Op::Call) continue;
            const auto& cn = i->calleeName;
            if (cn.find("push_back")    != std::string::npos ||
                cn.find("emplace_back") != std::string::npos ||
                cn.find("_M_insert")    != std::string::npos)
                return true;
        }
    }
    return false;
}

IteratorPatternRecovery::IteratorResult
IteratorPatternRecovery::recover(const ssa::SSAFunction& fn) const {
    IteratorResult r;
    r.isBeginEnd      = hasBeginEndPair(fn);
    r.isReverseIter   = hasReverseIter(fn);
    r.hasBackInserter = hasBackInserter(fn);

    if (r.isReverseIter) {
        r.rangeForForm = "for (auto& e : std::ranges::reverse_view(v))";
    } else if (r.isBeginEnd) {
        r.rangeForForm = "for (auto& e : v)";
    }

    if (r.hasBackInserter) {
        r.backInserter = "std::back_inserter(dst)";
    }

    return r;
}

} // namespace algo_recover
} // namespace retdec
