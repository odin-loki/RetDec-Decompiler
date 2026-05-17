/**
 * @file src/idiom_reconstruct/modulo_recovery.cpp
 * @brief Integer modulo-by-constant idiom recovery.
 *
 * ## Theory
 *
 * For `x % K`, compilers emit the division idiom followed by a multiply-back
 * and subtract:
 *
 *   q = x / K                 (the division idiom, already recovered)
 *   t = q * K                 (IMUL/MUL with immediate K)
 *   r = x - t                 (SUB)
 *
 * Alternatively, for K a power of 2:
 *   unsigned: r = x & (K-1)               (AND with mask)
 *   signed:   r = x - K * (x >> log2(K))  (sign-correct variant)
 *             or the classic:
 *               t = x >> 31               (sign bit)
 *               t2 = t >> (32-k)          (= 0 or K-1)
 *               t3 = x + t2
 *               r  = (t3 & -K) - t2       (used by GCC for signed pow-2 mod)
 *
 * ## Matching strategy
 *
 * Because modulo always follows division, the engine runs the modulo matcher
 * at a position where a DivisionMatcher result already occupies the earlier
 * instructions.  The modulo matcher looks for the MUL-back + SUB suffix
 * attached to a recently seen division result register.
 *
 * Since the engine scans linearly and the DivisionMatcher will have already
 * consumed its instructions, the modulo matcher only needs to look at the
 * NEXT few instructions after the division result.
 *
 * However, because the engine passes raw windows (no state between matchers),
 * we implement the modulo matcher to detect the FULL pattern:
 * division-idiom + mul-back + sub.  The engine's greedy scan will skip the
 * positions already matched by division, so in practice modulo is matched
 * *after* division in the same window pass when both appear together.
 *
 * Alternatively we match the standalone patterns (power-of-2 AND, signed
 * power-of-2) independently.
 *
 * ## Instruction sequences
 *
 * ### Unsigned modulo, power of 2: x & (K-1)
 *   [0]  AND  r, x, K-1
 *
 * ### Unsigned modulo, general K (appears after division sequence):
 *   [0..N-1] <division idiom producing q in qReg>
 *   [N]      IMUL/MUL t, qReg, K   (or: SHL + shifts if K is power of 2)
 *   [N+1]    SUB      r,  x,   t
 *
 * ### Signed modulo, power of 2 (GCC):
 *   [0]  SAR  t0, x, N-1           (= 0 or -1)
 *   [1]  SHR  t1, t0, N-k          (= 0 or K-1)
 *   [2]  ADD  t2, x, t1
 *   [3]  AND  t3, t2, -(int64_t)K  (= -K)
 *   [4]  SUB  r,  x, t3
 *   OR the equivalent:
 *   [0]  SAR  t0, x, N-1
 *   [1]  AND  t1, t0, K-1
 *   [2]  ADD  t2, x, t1
 *   [3]  AND  t3, t2, -(int64_t)K
 *   [4]  SUB  r, x, t3
 */

#include <memory>
#include "retdec/idiom_reconstruct/idiom_reconstruct.h"

#include <cstdint>
#include <optional>

namespace retdec {
namespace idiom_reconstruct {

namespace {

/**
 * Check whether `v` is a power of 2.
 */
static bool isPow2(uint64_t v) {
    return v >= 2 && (v & (v-1)) == 0;
}
static int log2Floor(uint64_t v) {
    int k = 0;
    while (v >>= 1) ++k;
    return k;
}

class ModuloMatcher : public IIdiomMatcher {
public:
    const char* name() const noexcept override { return "ModuloByConstant"; }
    std::size_t minWindowSize() const noexcept override { return 1; }

    std::optional<ReplacementNode> match(const InstrWindow& W,
                                          std::size_t        off,
                                          CompilerProfile    /*prof*/) const override
    {
        const std::size_t n = W.size();

        // ── Case 0: Unsigned power-of-2: AND r, x, K-1 ───────────────────────
        if (off < n &&
            W[off].op==IdiomOp::And && W[off].isImm(1))
        {
            uint64_t mask = (uint64_t)W[off].getImm(1);
            // mask must be K-1 where K is a power of 2
            uint64_t K = mask + 1;
            if (isPow2(K)) {
                ReplacementNode r;
                r.kind        = ReplacementKind::ModUnsigned;
                r.inputReg    = W[off].src0.reg;
                r.outputReg   = W[off].dst.reg;
                r.divisor     = (int64_t)K;
                r.operandWidth= W[off].src0.width;
                r.firstVma    = W[off].vma;
                r.lastVma     = W[off].vma;
                r.instrCount  = 1;
                return r;
            }
        }

        // ── Case 1: General modulo suffix: IMUL/MUL qReg, K + SUB r, x, t ────
        // This matches the tail of a combined division+modulo sequence.
        // The division matcher has already fired; we look for the MUL-back+SUB.
        if (off+1 < n) {
            bool isMul = (W[off].op==IdiomOp::IMul || W[off].op==IdiomOp::Mul);
            if (isMul && W[off].isImm(1)) {
                int64_t K = W[off].getImm(1);
                if (K >= 2 || K <= -2) {
                    uint32_t tReg = W[off].dst.reg;
                    // SUB r, x, t
                    if (W[off+1].op==IdiomOp::Sub && W[off+1].src1.reg==tReg) {
                        uint32_t xReg = W[off+1].src0.reg;
                        ReplacementNode r;
                        r.kind        = K > 0 ? ReplacementKind::ModUnsigned
                                               : ReplacementKind::ModSigned;
                        r.inputReg    = xReg;
                        r.outputReg   = W[off+1].dst.reg;
                        r.divisor     = K;
                        r.operandWidth= W[off].src0.width;
                        r.firstVma    = W[off].vma;
                        r.lastVma     = W[off+1].vma;
                        r.instrCount  = 2;
                        return r;
                    }
                }
            }
        }

        // ── Case 2: Signed power-of-2 mod (GCC 5-instruction form) ────────────
        // SAR t0, x, N-1; SHR t1, t0, N-k; ADD t2, x, t1; AND t3, t2, ~(K-1); SUB r, x, t3
        if (off+4 < n &&
            W[off].op==IdiomOp::Sar && W[off].isImm(1))
        {
            int N = (int)W[off].src0.width;
            if (W[off].getImm(1)==N-1) {
                uint32_t xReg = W[off].src0.reg;
                uint32_t t0   = W[off].dst.reg;
                // SHR t1, t0, N-k  (or AND t1, t0, K-1)
                bool usesShr = (W[off+1].op==IdiomOp::Shr || W[off+1].op==IdiomOp::And);
                if (usesShr && W[off+1].src0.reg==t0 && W[off+1].isImm(1)) {
                    int k = 0;
                    uint64_t K = 0;
                    if (W[off+1].op==IdiomOp::Shr) {
                        int shift2 = (int)W[off+1].getImm(1);
                        k = N - shift2;
                        K = (uint64_t)1 << k;
                    } else { // AND
                        uint64_t mask = (uint64_t)W[off+1].getImm(1);
                        K = mask + 1;
                        k = log2Floor(K);
                    }
                    if (isPow2(K) && k >= 1 && k < N) {
                        uint32_t t1 = W[off+1].dst.reg;
                        // ADD t2, x, t1
                        if (W[off+2].op==IdiomOp::Add &&
                            ((W[off+2].src0.reg==xReg && W[off+2].src1.reg==t1) ||
                             (W[off+2].src0.reg==t1   && W[off+2].src1.reg==xReg)))
                        {
                            uint32_t t2 = W[off+2].dst.reg;
                            // AND t3, t2, -K  (mask = ~(K-1) = -(int64_t)K)
                            if (W[off+3].op==IdiomOp::And &&
                                W[off+3].src0.reg==t2 && W[off+3].isImm(1) &&
                                (uint64_t)W[off+3].getImm(1) == (uint64_t)(-(int64_t)K))
                            {
                                uint32_t t3 = W[off+3].dst.reg;
                                // SUB r, x, t3
                                if (W[off+4].op==IdiomOp::Sub &&
                                    W[off+4].src0.reg==xReg &&
                                    W[off+4].src1.reg==t3)
                                {
                                    ReplacementNode r;
                                    r.kind        = ReplacementKind::ModSigned;
                                    r.inputReg    = xReg;
                                    r.outputReg   = W[off+4].dst.reg;
                                    r.divisor     = (int64_t)K;
                                    r.operandWidth= (uint32_t)N;
                                    r.firstVma    = W[off].vma;
                                    r.lastVma     = W[off+4].vma;
                                    r.instrCount  = 5;
                                    return r;
                                }
                            }
                        }
                    }
                }
            }
        }

        return std::nullopt;
    }
};

} // anon namespace

std::unique_ptr<IIdiomMatcher> makeModuloMatcher() {
    return std::make_unique<ModuloMatcher>();
}

} // namespace idiom_reconstruct
} // namespace retdec
