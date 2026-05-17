/**
 * @file src/idiom_reconstruct/bit_idioms.cpp
 * @brief Bit-manipulation and boolean conversion idiom recovery.
 *
 * ## Idioms matched
 *
 * ### 1. Lowest set bit isolation  →  x & -x
 *   Compilers emit:   NEG t, x;  AND r, x, t
 *   Or inline: x & (~x + 1)   = x & (-x)
 *   [0]  NEG  t, x       (t = -x = ~x + 1)
 *   [1]  AND  r, x, t
 *   → ReplacementKind::LowestSetBit
 *
 * ### 2. Clear lowest set bit  →  x & (x-1)
 *   [0]  SUB/ADD  t, x, -1   (t = x-1)  or  ADD t, x, imm(-1)
 *   [1]  AND      r, x, t
 *   → ReplacementKind::ClearLowestSetBit
 *
 * ### 3. ToBool  →  (bool)x   or  !!x
 *   x86 form: NEG t, x;  SBB r, r, r;  NEG r    (r = 0 or 1)
 *   Or: TEST x, x;  SETNE r   (one instruction with flags — we match
 *   the NEG+SBB+NEG triple since SETNE is captured by the flag system)
 *   [0]  NEG  t, x       (sets CF iff x!=0)
 *   [1]  SBB  r, r, r    (r = r - r - CF = 0 - CF = 0 or -1)
 *   [2]  NEG  r2, r      (r2 = 0 or 1)
 *   → ReplacementKind::ToBool
 *
 *   Simpler form (Clang/ICC): `x != 0` is sometimes emitted as:
 *   [0]  TEST x, x        (sets ZF)
 *   [1]  SETNE r          → this is just a compare, handled as IsZero/ToBool
 *   We handle the NOT+AND form for !x:
 *   [0]  NOT  t, x
 *   [1]  AND  r, t, 1    → r = 1 if x==0, 0 otherwise = !x = IsZero
 *   → ReplacementKind::IsZero
 *
 * ### 4. Popcount  →  __builtin_popcount(x)
 *   The parallel-bit-count idiom (from Hacker's Delight §5-1):
 *   x = x - ((x >> 1) & 0x55555555);
 *   x = (x & 0x33333333) + ((x >> 2) & 0x33333333);
 *   x = (x + (x >> 4)) & 0x0F0F0F0F;
 *   x = (x * 0x01010101) >> 24;
 *
 *   We recognise the magic constants:
 *     0x55555555 (= 0x5555...5, alternating bits)
 *     0x33333333 (= 0x3333...3, pairs)
 *     0x0F0F0F0F (= nibble mask)
 *     0x01010101 (= byte sum trick)
 *   and the shift amounts 1, 2, 4, 24.
 *
 *   A full match requires seeing at least 6-8 instructions with the right
 *   constants.  We use a state machine over the constants.
 *
 * ### 5. Byte-swap  →  __builtin_bswap32/64(x)
 *   x86 BSWAP: one instruction — the platform adapter should emit IdiomOp::Ror
 *   with src1 being a full-width rotate (e.g., 16 for 32-bit) or emit
 *   it directly with a special op.  Here we detect the software form:
 *
 *   32-bit byte-swap (used when BSWAP unavailable, e.g. 8086 compat):
 *   [0]  ROR  t0, x, 16          ; swap upper/lower 16-bit halves
 *   [1]  AND  t1, t0, 0xFF00FF   ; mask byte 0 and byte 2
 *   [2]  SHR  t2, t0, 8
 *   [3]  AND  t3, t2, 0xFF00FF   ; mask byte 1 and byte 3
 *   [4]  OR   r, t1, t3
 *
 *   We also recognise the single-instruction case from the platform adapter:
 *   [0]  Ror  r, x, 16   with width==16   → bswap16
 *   [0]  Ror  r, x, 16   with width==32   → bswap32 (and mask)
 */

#include <memory>
#include "retdec/idiom_reconstruct/idiom_reconstruct.h"
#include <cstdint>
#include <optional>

namespace retdec {
namespace idiom_reconstruct {

namespace {

// Magic constants for popcount parallel bit-count algorithm
static constexpr uint64_t k5555 = 0x5555555555555555ULL;
static constexpr uint64_t k3333 = 0x3333333333333333ULL;
static constexpr uint64_t k0F0F = 0x0F0F0F0F0F0F0F0FULL;
static constexpr uint64_t k0101 = 0x0101010101010101ULL;

// Truncate constant to N bits
static uint64_t trunc(uint64_t v, int N) {
    if (N >= 64) return v;
    return v & (((uint64_t)1 << N) - 1);
}

class BitIdiomMatcher : public IIdiomMatcher {
public:
    const char* name() const noexcept override { return "BitManipulation"; }
    std::size_t minWindowSize() const noexcept override { return 1; }

    std::optional<ReplacementNode> match(const InstrWindow& W,
                                          std::size_t        off,
                                          CompilerProfile    /*prof*/) const override
    {
        const std::size_t n = W.size();

        // ── 1. Lowest set bit: NEG t, x; AND r, x, t ─────────────────────────
        if (off+1 < n &&
            W[off].op==IdiomOp::Neg)
        {
            uint32_t xReg = W[off].src0.reg;
            uint32_t tReg = W[off].dst.reg;
            if (W[off+1].op==IdiomOp::And &&
                ((W[off+1].src0.reg==xReg && W[off+1].src1.reg==tReg) ||
                 (W[off+1].src0.reg==tReg && W[off+1].src1.reg==xReg)))
            {
                ReplacementNode r;
                r.kind        = ReplacementKind::LowestSetBit;
                r.inputReg    = xReg;
                r.outputReg   = W[off+1].dst.reg;
                r.operandWidth= W[off].src0.width;
                r.firstVma    = W[off].vma;
                r.lastVma     = W[off+1].vma;
                r.instrCount  = 2;
                return r;
            }
        }
        // Alternative: AND r, x, imm(-x treated as NEG)
        // (compiler sometimes folds NEG into an immediate negation in the AND)
        // Skip — too ambiguous without context.

        // ── 2. Clear lowest set bit: SUB t, x, 1; AND r, x, t ────────────────
        if (off+1 < n &&
            (W[off].op==IdiomOp::Sub || W[off].op==IdiomOp::Add))
        {
            uint32_t xReg = W[off].src0.reg;
            if (W[off].isImm(1)) {
                int64_t imm = W[off].getImm(1);
                // SUB x, 1  OR ADD x, -1
                bool isMinusOne = (W[off].op==IdiomOp::Sub && imm==1) ||
                                  (W[off].op==IdiomOp::Add && imm==-1);
                if (isMinusOne) {
                    uint32_t tReg = W[off].dst.reg;
                    if (W[off+1].op==IdiomOp::And &&
                        ((W[off+1].src0.reg==xReg && W[off+1].src1.reg==tReg) ||
                         (W[off+1].src0.reg==tReg && W[off+1].src1.reg==xReg)))
                    {
                        ReplacementNode r;
                        r.kind        = ReplacementKind::ClearLowestSetBit;
                        r.inputReg    = xReg;
                        r.outputReg   = W[off+1].dst.reg;
                        r.operandWidth= W[off].src0.width;
                        r.firstVma    = W[off].vma;
                        r.lastVma     = W[off+1].vma;
                        r.instrCount  = 2;
                        return r;
                    }
                }
            }
        }

        // ── 3a. ToBool (!!x): NEG t, x; SBB r, r, r; NEG r2, r ──────────────
        if (off+2 < n &&
            W[off].op==IdiomOp::Neg)
        {
            // The SBB instruction is not in our IdiomOp enum since it's x86-specific.
            // We rely on the platform adapter to emit it as a Sub with a special flag.
            // Here we check for the pattern: Sub with dst==src0 and src1==dst (r-r form).
            // This is the SBB r,r,r form.
            if (W[off+1].op==IdiomOp::Sub &&
                W[off+1].dst.reg==W[off+1].src0.reg &&
                W[off+1].src0.reg==W[off+1].src1.reg)
            {
                uint32_t sbbReg = W[off+1].dst.reg;
                if (W[off+2].op==IdiomOp::Neg &&
                    W[off+2].src0.reg==sbbReg)
                {
                    ReplacementNode r;
                    r.kind        = ReplacementKind::ToBool;
                    r.inputReg    = W[off].src0.reg;
                    r.outputReg   = W[off+2].dst.reg;
                    r.operandWidth= W[off].src0.width;
                    r.firstVma    = W[off].vma;
                    r.lastVma     = W[off+2].vma;
                    r.instrCount  = 3;
                    return r;
                }
            }
        }

        // ── 3b. IsZero (!x): NOT t, x; AND r, t, 1 ───────────────────────────
        if (off+1 < n &&
            W[off].op==IdiomOp::Not)
        {
            uint32_t tReg = W[off].dst.reg;
            if (W[off+1].op==IdiomOp::And &&
                W[off+1].src0.reg==tReg && W[off+1].isImm(1) &&
                W[off+1].getImm(1)==1)
            {
                ReplacementNode r;
                r.kind        = ReplacementKind::IsZero;
                r.inputReg    = W[off].src0.reg;
                r.outputReg   = W[off+1].dst.reg;
                r.operandWidth= W[off].src0.width;
                r.firstVma    = W[off].vma;
                r.lastVma     = W[off+1].vma;
                r.instrCount  = 2;
                return r;
            }
        }

        // ── 4. Popcount (parallel bit-count, 32-bit or 64-bit) ────────────────
        // We look for the sequence of magic-constant operations.
        // State machine: check for AND-0x5555 after SHR-1, then AND-0x3333 etc.
        if (off+5 < n) {
            if (auto pc = tryPopcount(W, off, n)) return pc;
        }

        // ── 5. Software byte-swap: ROR 16 + AND + SHR + AND + OR ─────────────
        if (off+4 < n &&
            W[off].op==IdiomOp::Ror && W[off].isImm(1) && W[off].getImm(1)==16)
        {
            if (auto bs = tryByteSwap32(W, off, n)) return bs;
        }

        // Single-instruction bswap-like (rotate by half word width)
        if (off < n &&
            W[off].op==IdiomOp::Ror && W[off].isImm(1))
        {
            int N = (int)W[off].src0.width;
            int rot = (int)W[off].getImm(1);
            if (N==16 && rot==8) {
                ReplacementNode r;
                r.kind        = ReplacementKind::ByteSwap;
                r.inputReg    = W[off].src0.reg;
                r.outputReg   = W[off].dst.reg;
                r.operandWidth= 16;
                r.firstVma    = W[off].vma;
                r.lastVma     = W[off].vma;
                r.instrCount  = 1;
                return r;
            }
        }

        return std::nullopt;
    }

private:
    // ── Popcount state machine ────────────────────────────────────────────────

    std::optional<ReplacementNode> tryPopcount(const InstrWindow& W,
                                                std::size_t off,
                                                std::size_t n) const
    {
        // We detect the 8-instruction 32-bit form:
        // [0]  SHR  t0, x,  1
        // [1]  AND  t1, t0, 0x55555555
        // [2]  SUB  t2, x,  t1
        // [3]  AND  t3, t2, 0x33333333
        // [4]  SHR  t4, t2, 2
        // [5]  AND  t5, t4, 0x33333333
        // [6]  ADD  t6, t3, t5
        // [7]  SHR  t7, t6, 4
        // [8]  ADD  t8, t6, t7
        // [9]  AND  t9, t8, 0x0F0F0F0F
        // [10] MUL  t10, t9, 0x01010101
        // [11] SHR  r,   t10, 24
        //
        // The first check is: SHR by 1 followed by AND with 0x55555555

        if (off+7 >= n) return std::nullopt;
        int N = (int)W[off].src0.width;
        if (N != 32 && N != 64) return std::nullopt;
        uint64_t mask5 = trunc(k5555, N);
        uint64_t mask3 = trunc(k3333, N);
        uint64_t maskF = trunc(k0F0F, N);
        uint64_t mask1 = trunc(k0101, N);
        int finalShift = (N==32) ? 24 : 56;

        std::size_t i = off;
        uint32_t xReg = W[i].src0.reg;

        // [0] SHR t0, x, 1
        if (W[i].op!=IdiomOp::Shr || !W[i].isImm(1) || W[i].getImm(1)!=1) return std::nullopt;
        uint32_t t0 = W[i].dst.reg; ++i;

        // [1] AND t1, t0, 0x5555...
        if (i>=n || W[i].op!=IdiomOp::And) return std::nullopt;
        if (W[i].src0.reg!=t0 || !W[i].isImm(1)) return std::nullopt;
        if ((uint64_t)W[i].getImm(1)!=mask5) return std::nullopt;
        uint32_t t1 = W[i].dst.reg; ++i;

        // [2] SUB t2, x, t1
        if (i>=n || W[i].op!=IdiomOp::Sub) return std::nullopt;
        if (W[i].src0.reg!=xReg || W[i].src1.reg!=t1) return std::nullopt;
        uint32_t t2 = W[i].dst.reg; ++i;

        // [3] AND t3, t2, 0x3333...
        if (i>=n || W[i].op!=IdiomOp::And) return std::nullopt;
        if (W[i].src0.reg!=t2 || !W[i].isImm(1)) return std::nullopt;
        if ((uint64_t)W[i].getImm(1)!=mask3) return std::nullopt;
        uint32_t t3 = W[i].dst.reg; ++i;

        // [4] SHR t4, t2, 2
        if (i>=n || W[i].op!=IdiomOp::Shr) return std::nullopt;
        if (W[i].src0.reg!=t2 || !W[i].isImm(1) || W[i].getImm(1)!=2) return std::nullopt;
        uint32_t t4 = W[i].dst.reg; ++i;

        // [5] AND t5, t4, 0x3333...
        if (i>=n || W[i].op!=IdiomOp::And) return std::nullopt;
        if (W[i].src0.reg!=t4 || !W[i].isImm(1)) return std::nullopt;
        if ((uint64_t)W[i].getImm(1)!=mask3) return std::nullopt;
        uint32_t t5 = W[i].dst.reg; ++i;

        // [6] ADD t6, t3, t5
        if (i>=n || W[i].op!=IdiomOp::Add) return std::nullopt;
        if (!((W[i].src0.reg==t3&&W[i].src1.reg==t5)||(W[i].src0.reg==t5&&W[i].src1.reg==t3))) return std::nullopt;
        uint32_t t6 = W[i].dst.reg; ++i;

        // [7] SHR t7, t6, 4
        if (i>=n || W[i].op!=IdiomOp::Shr) return std::nullopt;
        if (W[i].src0.reg!=t6 || !W[i].isImm(1) || W[i].getImm(1)!=4) return std::nullopt;
        uint32_t t7 = W[i].dst.reg; ++i;

        // [8] ADD t8, t6, t7
        if (i>=n || W[i].op!=IdiomOp::Add) return std::nullopt;
        if (!((W[i].src0.reg==t6&&W[i].src1.reg==t7)||(W[i].src0.reg==t7&&W[i].src1.reg==t6))) return std::nullopt;
        uint32_t t8 = W[i].dst.reg; ++i;

        // [9] AND t9, t8, 0x0F0F...
        if (i>=n || W[i].op!=IdiomOp::And) return std::nullopt;
        if (W[i].src0.reg!=t8 || !W[i].isImm(1)) return std::nullopt;
        if ((uint64_t)W[i].getImm(1)!=maskF) return std::nullopt;
        uint32_t t9 = W[i].dst.reg; ++i;

        // [10] MUL t10, t9, 0x01010101
        if (i>=n || (W[i].op!=IdiomOp::Mul && W[i].op!=IdiomOp::IMul)) return std::nullopt;
        if (W[i].src0.reg!=t9 || !W[i].isImm(1)) return std::nullopt;
        if ((uint64_t)W[i].getImm(1)!=mask1) return std::nullopt;
        uint32_t t10= W[i].dst.reg; ++i;

        // [11] SHR r, t10, finalShift
        if (i>=n || W[i].op!=IdiomOp::Shr) return std::nullopt;
        if (W[i].src0.reg!=t10 || !W[i].isImm(1) || W[i].getImm(1)!=finalShift) return std::nullopt;

        ReplacementNode r;
        r.kind        = ReplacementKind::Popcount;
        r.inputReg    = xReg;
        r.outputReg   = W[i].dst.reg;
        r.operandWidth= (uint32_t)N;
        r.firstVma    = W[off].vma;
        r.lastVma     = W[i].vma;
        r.instrCount  = i - off + 1;
        return r;
    }

    // ── Software bswap32: ROR+AND+SHR+AND+OR ─────────────────────────────────

    std::optional<ReplacementNode> tryByteSwap32(const InstrWindow& W,
                                                   std::size_t off,
                                                   std::size_t n) const
    {
        if (off+4 >= n) return std::nullopt;
        uint32_t xReg = W[off].src0.reg;
        uint32_t t0   = W[off].dst.reg;

        // AND t1, t0, 0x00FF00FF
        if (W[off+1].op!=IdiomOp::And || W[off+1].src0.reg!=t0 || !W[off+1].isImm(1)) return std::nullopt;
        if ((uint64_t)W[off+1].getImm(1)!=0x00FF00FFULL) return std::nullopt;
        uint32_t t1 = W[off+1].dst.reg;

        // SHR t2, t0, 8
        if (W[off+2].op!=IdiomOp::Shr || W[off+2].src0.reg!=t0 || !W[off+2].isImm(1) || W[off+2].getImm(1)!=8) return std::nullopt;
        uint32_t t2 = W[off+2].dst.reg;

        // AND t3, t2, 0x00FF00FF
        if (W[off+3].op!=IdiomOp::And || W[off+3].src0.reg!=t2 || !W[off+3].isImm(1)) return std::nullopt;
        if ((uint64_t)W[off+3].getImm(1)!=0x00FF00FFULL) return std::nullopt;
        uint32_t t3 = W[off+3].dst.reg;

        // OR r, t1, t3
        if (W[off+4].op!=IdiomOp::Or) return std::nullopt;
        if (!((W[off+4].src0.reg==t1&&W[off+4].src1.reg==t3)||(W[off+4].src0.reg==t3&&W[off+4].src1.reg==t1))) return std::nullopt;

        ReplacementNode r;
        r.kind        = ReplacementKind::ByteSwap;
        r.inputReg    = xReg;
        r.outputReg   = W[off+4].dst.reg;
        r.operandWidth= 32;
        r.firstVma    = W[off].vma;
        r.lastVma     = W[off+4].vma;
        r.instrCount  = 5;
        return r;
    }
};

} // anon namespace

std::unique_ptr<IIdiomMatcher> makeBitIdiomMatcher() {
    return std::make_unique<BitIdiomMatcher>();
}

} // namespace idiom_reconstruct
} // namespace retdec
