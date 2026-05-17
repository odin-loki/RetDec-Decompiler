/**
 * @file src/sort_detect/radixsort_detect.cpp
 * @brief Radix sort detector implementation.
 *
 * Radix sort is a non-comparison sort.  Its key structural property is that
 * it contains **zero element-comparison instructions** (no CMP between
 * array elements, only comparisons on loop counters and pointer arithmetic).
 *
 * Four structural passes per digit:
 *
 *   1. **Digit extraction** — `(element >> k) & mask` where `k` is a constant
 *      multiple of the digit width (e.g. 0, 8, 16, 24 for 8-bit radix).
 *      In IR: Shr(load, constant) + And(result, mask_constant).
 *
 *   2. **Histogram accumulation** — a loop over the input array that reads each
 *      element, extracts the digit, and increments `count[digit]`.
 *      In IR: load + digit extraction + load from small fixed-size array +
 *             Add + store (increment).
 *
 *   3. **Prefix-sum pass** — a loop over the histogram array (of radix size)
 *      converting frequencies to cumulative offsets.
 *      In IR: a short loop (≤ 256 iterations) over a small array with running
 *             sum via Add.
 *
 *   4. **Scatter (redistribution)** — a loop over the input that places each
 *      element into the output buffer at position `offset[digit]++`.
 *      In IR: digit extraction + load from offset array + Add + Store (scatter).
 *
 * The four passes repeat for each digit position (2–8 times typically).
 *
 * Confidence:
 *   zero element comparisons               +0.35
 *   digit extraction (Shr + And with const)+0.25
 *   histogram accumulation pattern         +0.20
 *   scatter pass pattern                   +0.20
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

// Radix sort has zero element-comparison instructions.
// We check that the number of Compare instructions is zero or is used
// ONLY for loop-counter checks (not for element values).
// Heuristic: any Compare instruction is only in blocks that also contain
// Add or Sub (loop counter manipulation), not Load (element access).
static int countElementComparisons(const ssa::SSAFunction& fn) {
    int elemCmps = 0;
    for (uint32_t b = 0; b < fn.blockCount(); ++b) {
        const auto* blk = fn.block(b);
        if (!blk) continue;
        bool hasLoad = false, hasCmp = false;
        for (const auto* instr : blk->instrs) {
            if (!instr) continue;
            if (instr->op == ssa::IrInstr::Op::Load)    hasLoad = true;
            if (instr->op == ssa::IrInstr::Op::Compare) hasCmp  = true;
        }
        if (hasLoad && hasCmp) ++elemCmps; // potential element comparison block
    }
    return elemCmps;
}

// Digit extraction: Shr + And with a constant mask.
static bool hasDigitExtraction(const ssa::SSAFunction& fn) {
    // We look for a Shr instruction where the shift amount is a constant
    // multiple of 8 (or 4 for nibble radix), followed by an And with a
    // constant mask (0xff, 0xf, 0xffff, etc.).
    bool hasShr = false, hasAndMask = false;
    for (uint32_t b = 0; b < fn.blockCount(); ++b) {
        const auto* blk = fn.block(b);
        if (!blk) continue;
        for (const auto* instr : blk->instrs) {
            if (!instr) continue;
            if (instr->op == ssa::IrInstr::Op::Shr ||
                instr->op == ssa::IrInstr::Op::Sar) {
                // Check if shift amount is a constant multiple of 4 or 8.
                if (instr->uses.size() >= 2) {
                    const auto* sv = fn.value(instr->uses[1].valueId);
                    if (sv && sv->kind == ssa::ValueKind::Immediate) {
                        uint64_t imm = sv->imm;
                        if (imm > 0 && (imm % 4 == 0 || imm % 8 == 0))
                            hasShr = true;
                    }
                }
            }
            if (instr->op == ssa::IrInstr::Op::And) {
                // Check for a mask constant like 0xff, 0xf, 0xffff, 0xffffffff.
                for (const auto& use : instr->uses) {
                    const auto* sv = fn.value(use.valueId);
                    if (!sv || sv->kind != ssa::ValueKind::Immediate) continue;
                    uint64_t imm = sv->imm;
                    if (imm == 0xf || imm == 0xff || imm == 0xffff ||
                        imm == 0xffffffff || imm == 0xfff)
                        hasAndMask = true;
                }
            }
        }
    }
    return hasShr && hasAndMask;
}

// Histogram accumulation: we see a load, digit extraction (Shr+And),
// then another load from a small array, Add +1, Store.
// Heuristic: total number of Load + Store pairs is >= 3 (loads from input
// + loads from histogram + stores to histogram).
static bool hasHistogramAccumulation(const ssa::SSAFunction& fn) {
    int loads  = countOp(fn, ssa::IrInstr::Op::Load);
    int stores = countOp(fn, ssa::IrInstr::Op::Store);
    int adds   = countOp(fn, ssa::IrInstr::Op::Add);
    // Histogram needs reads and writes; we expect at least 2 loads + 1 store
    // per digit pass.  With 2+ digit passes: >= 4 loads, >= 2 stores, >= 2 adds.
    return loads >= 4 && stores >= 2 && adds >= 2;
}

// Prefix-sum pass: a short loop over a fixed-size array with a running sum.
// Heuristic: Add instruction used to accumulate into a variable that feeds
// a Store whose address advances by a fixed stride.
// Simpler check: phi nodes (loop header) + Add + Store pattern.
static bool hasPrefixSumPass(const ssa::SSAFunction& fn) {
    int phis   = 0;
    for (const auto& phi : fn.phis()) if (phi) ++phis;
    int adds   = countOp(fn, ssa::IrInstr::Op::Add);
    int stores = countOp(fn, ssa::IrInstr::Op::Store);
    // A prefix-sum pass needs a loop (phi) with accumulation (Add) and write-back (Store).
    return phis >= 1 && adds >= 1 && stores >= 1;
}

// Scatter pass: similar to histogram but writes elements to output positions.
// Key distinction from histogram: writes are of element-sized values
// (not just an incremented counter).
// Heuristic: there are at least 2 distinct Store patterns in the function.
static bool hasScatterPass(const ssa::SSAFunction& fn) {
    int stores = countOp(fn, ssa::IrInstr::Op::Store);
    int loads  = countOp(fn, ssa::IrInstr::Op::Load);
    // Scatter writes elements → needs loads (read input) and stores (write output).
    return stores >= 3 && loads >= 4;
}

} // anonymous namespace

// ─── RadixsortDetector ────────────────────────────────────────────────────────

int RadixsortDetector::countComparisonInstrs(const ssa::SSAFunction& fn) const {
    return countElementComparisons(fn);
}

bool RadixsortDetector::hasDigitExtraction(const ssa::SSAFunction& fn) const {
    return ::retdec::sort_detect::hasDigitExtraction(fn);
}

bool RadixsortDetector::hasHistogramAccumulation(const ssa::SSAFunction& fn) const {
    return ::retdec::sort_detect::hasHistogramAccumulation(fn);
}

bool RadixsortDetector::hasPrefixSumPass(const ssa::SSAFunction& fn) const {
    return ::retdec::sort_detect::hasPrefixSumPass(fn);
}

bool RadixsortDetector::hasScatterPass(const ssa::SSAFunction& fn) const {
    return ::retdec::sort_detect::hasScatterPass(fn);
}

SortResult RadixsortDetector::detect(const ssa::SSAFunction& fn) const {
    SortResult result;
    result.algorithm = SortAlgorithm::Radixsort;

    float score = 0.0f;

    // Zero element comparisons — strongest signal.
    int elemCmps = countComparisonInstrs(fn);
    if (elemCmps == 0)            score += 0.35f;
    else if (elemCmps <= 2)       score += 0.15f; // partial credit

    if (hasDigitExtraction(fn))   score += 0.25f;
    if (hasHistogramAccumulation(fn)) score += 0.20f;
    if (hasScatterPass(fn))       score += 0.20f;

    result.confidence = score > 1.0f ? 1.0f : score;

    // Radix sort is not in the STL so no standard compiler variant.
    result.compilerVariant = CompilerVariant::Unknown;

    return result;
}

} // namespace sort_detect
} // namespace retdec
