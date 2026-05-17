/**
 * @file src/sort_detect/partition_detect.cpp
 * @brief Partition loop fingerprint analysis.
 *
 * ## What we look for
 *
 * A Hoare-style partition loop has the following structural properties
 * when compiled to IR:
 *
 *  1. **Two loop induction variables** that start at opposite ends of the
 *     range and converge toward each other.  In SSA form these manifest as
 *     phi nodes at the loop header fed by an increment and a decrement.
 *
 *  2. **An element comparison** — a Compare instruction (`CmpLt`, `CmpGt`,
 *     `CmpLe`, `CmpGe`) applied to values loaded from the two positions.
 *
 *  3. **A swap pattern** — two (or three, for a tmp-register swap) Store
 *     instructions that exchange the elements at the two positions.
 *
 *  4. **Convergence exit condition** — the loop exits when the left index
 *     meets or crosses the right index.
 *
 * A Lomuto-style partition loop uses only one advancing index and a pivot
 * element; we score it with slightly lower confidence (0.4 vs 0.5).
 *
 * ## Scoring
 *
 *   comparison instruction found          +0.30
 *   swap pattern (3 stores in sequence)   +0.25
 *   two converging phi nodes              +0.25
 *   convergence exit branch               +0.20
 *
 * Max = 1.0 (rare; sum is capped at 1.0).
 */

#include "retdec/sort_detect/sort_detect.h"
#include "retdec/ssa/ssa.h"

#include <unordered_set>

namespace retdec {
namespace sort_detect {

namespace {

// Count instructions of a given opcode in all blocks of fn.
static int countOp(const ssa::SSAFunction& fn, ssa::IrInstr::Op op) {
    int n = 0;
    for (uint32_t bid = 0; bid < fn.blockCount(); ++bid) {
        const auto* blk = fn.block(bid);
        if (!blk) continue;
        for (const auto* instr : blk->instrs)
            if (instr && instr->op == op) ++n;
    }
    return n;
}

// Count Store instructions in a function.
static int countStores(const ssa::SSAFunction& fn) {
    return countOp(fn, ssa::IrInstr::Op::Store);
}

// Count Compare instructions (CMP / TEST equivalents).
static int countCompares(const ssa::SSAFunction& fn) {
    return countOp(fn, ssa::IrInstr::Op::Compare);
}

// Count Add instructions (advancing indices).
static int countAdds(const ssa::SSAFunction& fn) {
    return countOp(fn, ssa::IrInstr::Op::Add);
}

// Count Sub instructions (decrementing indices).
static int countSubs(const ssa::SSAFunction& fn) {
    return countOp(fn, ssa::IrInstr::Op::Sub);
}

// Count phi nodes in the function.
static int countPhis(const ssa::SSAFunction& fn) {
    int n = 0;
    for (const auto& phi : fn.phis()) {
        if (phi) ++n;
    }
    return n;
}

// Count conditional branches (CondBranch) in the function.
static int countCondBranches(const ssa::SSAFunction& fn) {
    return countOp(fn, ssa::IrInstr::Op::CondBranch);
}

// Heuristic: does this function contain a recognisable swap sequence?
// A swap of three elements uses 3 loads and 3 stores (tmp = a; a = b; b = tmp)
// but in modern compilers it often uses 2 loads and 2 stores with a register
// holding tmp.  We look for ≥ 2 stores within a single basic block.
static bool blockHasSwapPattern(const ssa::BasicBlock& blk) {
    int stores = 0;
    for (const auto* instr : blk.instrs) {
        if (instr && instr->op == ssa::IrInstr::Op::Store) ++stores;
    }
    return stores >= 2;
}

static bool hasSwapPatternAnyBlock(const ssa::SSAFunction& fn) {
    for (uint32_t bid = 0; bid < fn.blockCount(); ++bid) {
        const auto* blk = fn.block(bid);
        if (blk && blockHasSwapPattern(*blk)) return true;
    }
    return false;
}

// A partition loop typically has:
//   - At least 1 Compare
//   - At least 2 Stores (swap)
//   - Both Add and Sub (converging indices)
//   - At least 2 phi nodes (loop header merges for both indices)
//   - At least 2 CondBranch (element comparison + convergence check)
static float scoreHoare(const ssa::SSAFunction& fn) {
    float score = 0.0f;

    const int cmp  = countCompares(fn);
    const int str  = countStores(fn);
    const int phi  = countPhis(fn);
    const int add  = countAdds(fn);
    const int sub  = countSubs(fn);
    const int cb   = countCondBranches(fn);
    const bool swp = hasSwapPatternAnyBlock(fn);

    if (cmp >= 1)          score += 0.30f;
    if (swp)               score += 0.25f;
    if (phi >= 2 && add >= 1 && sub >= 1) score += 0.25f;
    if (cb >= 2)           score += 0.20f;

    return score > 1.0f ? 1.0f : score;
}

} // anonymous namespace

// ─── PartitionFingerprint ─────────────────────────────────────────────────────

bool PartitionFingerprint::hasConvergingIndices(const ssa::SSAFunction& fn) const {
    // Converging indices: function has phi nodes and both Add + Sub.
    return countPhis(fn) >= 2 &&
           countAdds(fn)  >= 1 &&
           countSubs(fn)  >= 1;
}

bool PartitionFingerprint::hasSwapPattern(const ssa::SSAFunction& fn) const {
    return hasSwapPatternAnyBlock(fn);
}

float PartitionFingerprint::scorePartition(const ssa::SSAFunction& fn) const {
    return scoreHoare(fn);
}

PartitionEvidence PartitionFingerprint::analyse(const ssa::SSAFunction& fn) const {
    PartitionEvidence ev;
    ev.confidence   = scorePartition(fn);
    ev.found        = ev.confidence >= 0.30f;
    ev.isHoareStyle = hasConvergingIndices(fn);

    // Find the first Compare instruction as the notional cmp site.
    for (uint32_t bid = 0; bid < fn.blockCount(); ++bid) {
        const auto* blk = fn.block(bid);
        if (!blk) continue;
        for (const auto* instr : blk->instrs) {
            if (instr && instr->op == ssa::IrInstr::Op::Compare) {
                ev.cmpInstrId = instr->id;
                break;
            }
        }
        if (ev.cmpInstrId != UINT32_MAX) break;
    }

    // Find the first Store in a swap-pattern block as the swap site.
    for (uint32_t bid = 0; bid < fn.blockCount(); ++bid) {
        const auto* blk = fn.block(bid);
        if (!blk || !blockHasSwapPattern(*blk)) continue;
        for (const auto* instr : blk->instrs) {
            if (instr && instr->op == ssa::IrInstr::Op::Store) {
                ev.swapInstrId = instr->id;
                break;
            }
        }
        if (ev.swapInstrId != UINT32_MAX) break;
    }

    return ev;
}

// ─── SiftDownFingerprint ──────────────────────────────────────────────────────

bool SiftDownFingerprint::hasChildIndexArithmetic(const ssa::SSAFunction& fn) const {
    // 2*i+1 decomposes as: Mul(i, 2) + 1  OR  Shl(i, 1) | 1  OR  Add(Add(i,i),1)
    // We look for a Mul instruction with an immediate 2 anywhere in the function,
    // or an Shl(x,1) pattern.  This is a necessary (not sufficient) condition.

    for (uint32_t bid = 0; bid < fn.blockCount(); ++bid) {
        const auto* blk = fn.block(bid);
        if (!blk) continue;
        for (const auto* instr : blk->instrs) {
            if (!instr) continue;
            // Shl x, 1  — same as 2*x
            if (instr->op == ssa::IrInstr::Op::Shl && !instr->uses.empty()) {
                // Check if any use is a constant 1 (the shift amount).
                for (const auto& u : instr->uses) {
                    const auto* sv = fn.value(u.valueId);
                    if (sv && sv->kind == ssa::ValueKind::Immediate && sv->imm == 1)
                        return true;
                }
            }
            // Mul x, 2
            if (instr->op == ssa::IrInstr::Op::Mul && instr->uses.size() >= 2) {
                for (std::size_t ui = 0; ui < 2; ++ui) {
                    const auto* sv = fn.value(instr->uses[ui].valueId);
                    if (sv && sv->kind == ssa::ValueKind::Immediate && sv->imm == 2)
                        return true;
                }
            }
        }
    }
    return false;
}

bool SiftDownFingerprint::hasMaxChildSelection(const ssa::SSAFunction& fn) const {
    // Max-child selection: a CondBranch that chooses between two load results.
    // Heuristic: function has a Compare followed by a CondBranch in the same block.
    for (uint32_t bid = 0; bid < fn.blockCount(); ++bid) {
        const auto* blk = fn.block(bid);
        if (!blk) continue;
        bool hasCmp = false;
        for (const auto* instr : blk->instrs) {
            if (!instr) continue;
            if (instr->op == ssa::IrInstr::Op::Compare)    hasCmp = true;
            if (instr->op == ssa::IrInstr::Op::CondBranch && hasCmp) return true;
        }
    }
    return false;
}

SiftDownEvidence SiftDownFingerprint::analyse(const ssa::SSAFunction& fn) const {
    SiftDownEvidence ev;
    ev.hasLeftArith    = hasChildIndexArithmetic(fn);
    ev.hasRightArith   = ev.hasLeftArith; // symmetric — same arithmetic pattern
    ev.hasMaxSelect    = hasMaxChildSelection(fn);
    ev.hasConditionalSwap = hasSwapPatternAnyBlock(fn);

    int score = 0;
    if (ev.hasLeftArith)         ++score;
    if (ev.hasMaxSelect)         ++score;
    if (ev.hasConditionalSwap)   ++score;

    ev.confidence = static_cast<float>(score) / 3.0f;
    ev.found      = score >= 2;
    return ev;
}

// ─── RecursiveHalvingFingerprint ──────────────────────────────────────────────

RecursiveHalvingEvidence RecursiveHalvingFingerprint::analyse(
        const ssa::SSAFunction& fn) const {
    RecursiveHalvingEvidence ev;
    const std::string& selfName = fn.name();
    if (selfName.empty()) return ev;

    // Count self-calls.
    for (uint32_t bid = 0; bid < fn.blockCount(); ++bid) {
        const auto* blk = fn.block(bid);
        if (!blk) continue;
        for (const auto* instr : blk->instrs) {
            if (!instr || instr->op != ssa::IrInstr::Op::Call) continue;
            if (instr->calleeName == selfName) ++ev.selfCallCount;
        }
    }

    ev.found = ev.selfCallCount >= 2;

    // Halving confirmed heuristic: there are Add instructions (computing mid)
    // and the function has sub-ranges as arguments to recursive calls.
    // We use Add+Shr presence as a proxy for mid-point computation.
    int addCount = countAdds(fn);
    int shrCount = countOp(fn, ssa::IrInstr::Op::Shr);
    ev.halvingConfirmed = ev.found && (addCount >= 1) && (shrCount >= 1 || addCount >= 2);

    return ev;
}

// ─── InsertionSortFingerprint ─────────────────────────────────────────────────

bool InsertionSortFingerprint::hasBackwardShiftLoop(const ssa::SSAFunction& fn) const {
    // An insertion sort inner loop:
    //  - Decrements a loop variable (Sub by 1 or Add by -1).
    //  - Loads an element, compares, stores shifted element.
    //  - Typically 3-6 basic blocks.
    return countSubs(fn) >= 1 &&
           countCompares(fn) >= 1 &&
           countStores(fn) >= 1 &&
           fn.blockCount() >= 3;
}

bool InsertionSortFingerprint::hasThresholdGuard(const ssa::SSAFunction& fn,
                                                   int& threshold) const {
    // A threshold guard manifests as a Compare + CondBranch near the function
    // entry that uses a small immediate constant (8–32) as the threshold.
    const auto* entry = fn.block(fn.entryId());
    if (!entry) return false;

    bool hasCmp = false;
    for (const auto* instr : entry->instrs) {
        if (!instr) continue;
        if (instr->op == ssa::IrInstr::Op::Compare) hasCmp = true;
        if (hasCmp && instr->op == ssa::IrInstr::Op::CondBranch) {
            // Look for a small immediate in the Compare's operands.
            // Walk backwards to find the Compare.
            for (const auto* i2 : entry->instrs) {
                if (!i2 || i2->op != ssa::IrInstr::Op::Compare) continue;
                for (const auto& use : i2->uses) {
                    const auto* val = fn.value(use.valueId);
                    if (val && val->kind == ssa::ValueKind::Immediate) {
                        uint64_t imm = val->imm;
                        if (imm >= 4 && imm <= 64) {
                            threshold = static_cast<int>(imm);
                            return true;
                        }
                    }
                }
            }
            return false;
        }
    }
    return false;
}

InsertionSortEvidence InsertionSortFingerprint::analyse(
        const ssa::SSAFunction& fn) const {
    InsertionSortEvidence ev;
    ev.found = hasBackwardShiftLoop(fn);
    if (ev.found) {
        ev.confidence = 0.5f;
        ev.hasThresholdGuard = hasThresholdGuard(fn, ev.threshold);
        if (ev.hasThresholdGuard) ev.confidence += 0.3f;
    }
    return ev;
}

} // namespace sort_detect
} // namespace retdec
