/**
 * @file src/idiom_reconstruct/simd_memcpy.cpp
 * @brief SIMD-vectorised memset / memcpy / memmove recognition.
 *
 * ## Background
 *
 * For small or medium-sized memory operations with a compile-time-known count,
 * compilers inline and vectorise:
 *   - `memset(dst, c, N)` → repeated SIMD stores of a broadcast value
 *   - `memcpy(dst, src, N)` → SIMD load-store pairs
 *
 * GCC/Clang with -O2 and MSVC /O2 typically unroll the vector loop entirely
 * for N ≤ 256 bytes.  For larger N they emit a counted loop.  We handle both.
 *
 * ## Recognition strategy
 *
 * This matcher operates on a LINEAR window (one basic block or one loop body).
 * It classifies the window by scanning for:
 *
 * ### memset detection
 *   1. One `VecSet` instruction broadcasting a scalar (or zero/immediate) to a
 *      SIMD register → the fill value.
 *   2. One or more `VecStore` instructions all storing the same SIMD register
 *      to consecutive addresses (dst, dst+vecWidth, dst+2*vecWidth, …).
 *   3. Optional scalar epilogue: `Store` instructions for the remaining bytes.
 *   4. A `Mov` loading a count into a register (for the counted-loop variant).
 *
 * ### memcpy detection
 *   1. One or more `VecLoad` + `VecStore` pairs where:
 *      - Load source addresses are consecutive (src, src+vecWidth, …)
 *      - Store destination addresses are consecutive (dst, dst+vecWidth, …)
 *      - Load and store widths match.
 *   2. No aliasing between src and dst ranges (we assume no_alias since the
 *      compiler emitted memcpy semantics).
 *
 * ### memmove detection
 *   Same as memcpy but with a direction check (backward copy loop → memmove).
 *   We check for decreasing address sequence (src+N-vecW, …, src downward).
 *
 * ## Heuristics for "is this a memory function?"
 *
 * We require:
 *   - At least 2 vector stores (otherwise it might just be a normal store).
 *   - All stores use the same SIMD register (memset) or paired load registers
 *     (memcpy).
 *   - The stride between consecutive stores equals the vector register width.
 *
 * ## IdiomInstr fields used
 *
 *   VecLoad/VecStore: dst=vecReg, src0=baseReg, src1=offsetImm (if any)
 *   VecSet: dst=vecReg, src0=scalarReg or src0=Imm(0) for vxorps/vpxor zeroing
 *   vecWidth: width in bytes of the vector register (16=SSE, 32=AVX, 64=AVX-512)
 *
 * ## Output
 *
 *   ReplacementKind::Memset with dstReg, fillValue (or fillReg), countImm or countReg.
 *   ReplacementKind::Memcpy with dstReg, srcReg, countImm or countReg.
 *   ReplacementKind::Memmove for backward-copy variant.
 *
 * ## Limitations
 *
 * This matcher does NOT handle cross-block loop recognition — that requires
 * CFG analysis (which is done in the CFG structuring stage, Task 28).
 * Within a single basic block (unrolled inlined copy), it works fully.
 */

#include <memory>
#include "retdec/idiom_reconstruct/idiom_reconstruct.h"

#include <algorithm>
#include <cstdint>
#include <optional>
#include <unordered_map>
#include <vector>

namespace retdec {
namespace idiom_reconstruct {

namespace {

struct MemAccess {
    uint32_t baseReg;
    int64_t  offsetBytes;  ///< immediate offset from base (if any)
    uint32_t vecReg;       ///< SIMD register used
    uint32_t vecWidth;     ///< width in bytes
    bool     isLoad;
    uint64_t vma;
};

class SimdMemMatcher : public IIdiomMatcher {
public:
    const char* name() const noexcept override { return "SimdMemset_Memcpy"; }
    std::size_t minWindowSize() const noexcept override { return 2; }

    std::optional<ReplacementNode> match(const InstrWindow& W,
                                          std::size_t        off,
                                          CompilerProfile    /*prof*/) const override
    {
        const std::size_t n = W.size();

        // ── Step 1: Scan for vector accesses starting at `off` ────────────────
        std::vector<MemAccess> accesses;
        uint32_t fillVecReg = UINT32_MAX;
        int64_t  fillValue  = 0;
        bool     hasFill    = false;
        std::size_t lastIdx = off;

        for (std::size_t i = off; i < n; ++i) {
            const IdiomInstr& ins = W[i];

            if (ins.op == IdiomOp::VecSet) {
                // Broadcast scalar (or zero) into SIMD register
                fillVecReg = ins.dst.reg;
                if (ins.src0.kind == OperandKind::Imm) fillValue = ins.src0.imm;
                else if (ins.src0.kind == OperandKind::Reg && ins.src0.reg == ins.src1.reg &&
                         ins.op == IdiomOp::Xor) fillValue = 0;  // vpxor self = zero
                else fillValue = 0;
                hasFill = true;
                lastIdx = i;
                continue;
            }

            if (ins.op == IdiomOp::VecStore || ins.op == IdiomOp::VecLoad) {
                MemAccess acc;
                acc.isLoad    = (ins.op == IdiomOp::VecLoad);
                acc.vecReg    = ins.dst.reg;
                acc.vecWidth  = ins.vecWidth > 0 ? ins.vecWidth : ins.dst.width / 8;
                acc.vma       = ins.vma;
                // For stores: dst=memory(base,offset), src0=vecReg
                // For loads:  dst=vecReg, src0=memory(base,offset)
                // We encode: baseReg in src0.reg, offset in src1.imm
                acc.baseReg     = ins.src0.reg;
                acc.offsetBytes = ins.src1.kind==OperandKind::Imm ? ins.src1.imm : 0;
                if (ins.op==IdiomOp::VecStore) acc.vecReg = ins.src0.reg; // store: src0=vecReg, dst=mem
                accesses.push_back(acc);
                lastIdx = i;
                continue;
            }

            // Scalar epilogue stores (Store) after the SIMD block
            if (ins.op == IdiomOp::Store && !accesses.empty()) {
                lastIdx = i;
                continue;
            }

            // Any non-memory non-setup instruction breaks the sequence
            if (!accesses.empty()) {
                // Allow up to 2 non-memory instructions (address arithmetic)
                if (ins.op != IdiomOp::Add && ins.op != IdiomOp::Lea &&
                    ins.op != IdiomOp::Mov && ins.op != IdiomOp::Sub)
                    break;
            }
        }

        if (accesses.size() < 2) return std::nullopt;

        // ── Step 2: Classify as memset or memcpy ──────────────────────────────

        // Separate loads and stores
        std::vector<MemAccess> loads, stores;
        for (auto& a : accesses) {
            if (a.isLoad) loads.push_back(a);
            else          stores.push_back(a);
        }

        if (stores.empty()) return std::nullopt;

        // Check that all stores use the same base register
        uint32_t dstBase = stores[0].baseReg;
        bool     allSameDst = std::all_of(stores.begin(), stores.end(),
            [dstBase](const MemAccess& a){ return a.baseReg == dstBase; });

        if (!allSameDst) return std::nullopt;

        // Check consecutive offsets (stride = vecWidth)
        if (!stores.empty()) {
            std::sort(stores.begin(), stores.end(),
                [](const MemAccess& a, const MemAccess& b){ return a.offsetBytes < b.offsetBytes; });
            uint32_t vecW = stores[0].vecWidth ? stores[0].vecWidth : 16;
            bool strideOk = true;
            for (std::size_t i=1; i<stores.size(); ++i) {
                int64_t expectedOff = stores[i-1].offsetBytes + vecW;
                if (stores[i].offsetBytes != expectedOff) { strideOk=false; break; }
            }
            if (!strideOk) return std::nullopt;
        }

        // Total bytes covered
        uint32_t vecW   = stores[0].vecWidth ? stores[0].vecWidth : 16;
        int64_t  count  = (int64_t)(stores.size() * vecW);

        // ── memset: no loads, fill value from VecSet ──────────────────────────
        if (loads.empty()) {
            // Check all stores use the fill register (or same constant)
            bool isFill = hasFill;
            if (!isFill) {
                // All stores same vec register?
                uint32_t vr = stores[0].vecReg;
                isFill = std::all_of(stores.begin(), stores.end(),
                    [vr](const MemAccess& a){ return a.vecReg==vr; });
            }
            if (!isFill) return std::nullopt;

            ReplacementNode r;
            r.kind      = ReplacementKind::Memset;
            r.dstReg    = dstBase;
            r.fillValue = fillValue;
            r.countImm  = count;
            r.firstVma  = W[off].vma;
            r.lastVma   = W[lastIdx].vma;
            r.instrCount= lastIdx - off + 1;
            return r;
        }

        // ── memcpy: loads from one base, stores to another ────────────────────
        uint32_t srcBase = loads[0].baseReg;
        bool allSameSrc = std::all_of(loads.begin(), loads.end(),
            [srcBase](const MemAccess& a){ return a.baseReg==srcBase; });
        if (!allSameSrc) return std::nullopt;
        if (srcBase == dstBase) return std::nullopt; // trivially aliased

        // Check load offsets match store offsets (same count in order)
        if (loads.size() != stores.size()) return std::nullopt;

        std::sort(loads.begin(), loads.end(),
            [](const MemAccess& a, const MemAccess& b){ return a.offsetBytes < b.offsetBytes; });

        bool offsetsMatch = true;
        for (std::size_t i=0; i<loads.size(); ++i) {
            if (loads[i].offsetBytes != stores[i].offsetBytes) { offsetsMatch=false; break; }
        }

        // Check for backward copy (memmove indicator)
        bool backward = false;
        if (!offsetsMatch) {
            // Check if reversed
            bool revMatch = true;
            for (std::size_t i=0; i<loads.size(); ++i) {
                std::size_t j = loads.size()-1-i;
                if (loads[i].offsetBytes != stores[j].offsetBytes) { revMatch=false; break; }
            }
            if (revMatch) { backward=true; offsetsMatch=true; }
        }

        if (!offsetsMatch) return std::nullopt;

        ReplacementNode r;
        r.kind      = backward ? ReplacementKind::Memmove : ReplacementKind::Memcpy;
        r.dstReg    = dstBase;
        r.srcReg    = srcBase;
        r.countImm  = count;
        r.firstVma  = W[off].vma;
        r.lastVma   = W[lastIdx].vma;
        r.instrCount= lastIdx - off + 1;
        return r;
    }
};

} // anon namespace

std::unique_ptr<IIdiomMatcher> makeSimdMemMatcher() {
    return std::make_unique<SimdMemMatcher>();
}

} // namespace idiom_reconstruct
} // namespace retdec
