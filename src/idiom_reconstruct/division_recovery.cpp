/**
 * @file src/idiom_reconstruct/division_recovery.cpp
 * @brief Integer division-by-constant idiom recovery.
 *
 * ## Theory
 *
 * For any divisor K in [2, 2^31-1], a compiler can compute `x / K` using a
 * multiply-by-magic-number + shift sequence that runs in ~3 cycles on modern
 * hardware, versus the 20-90 cycle latency of the hardware IDIV instruction.
 *
 * The transformation was described by Torbjörn Granlund and Peter L. Montgomery
 * (PLDI 1994: "Division by invariant integers using multiplication") and
 * independently by Henry S. Warren Jr. (Hacker's Delight, ch. 10).
 *
 * ### Unsigned 32-bit division by K (GCC/Clang/MSVC)
 *
 *   Choose N = 32, then find the smallest k ≥ 0 such that:
 *     2^(N+k) / K  is an integer OR we can use the round-up trick.
 *
 *   Case 1 — K is a power of 2:
 *     q = x >> log2(K)     (SHR)
 *     trivially recovered.
 *
 *   Case 2 — K is NOT a power of 2:
 *     M = ceil(2^(N+k) / K)            (the "magic number")
 *     q_approx = MULHU(x, M) >> k      (high half of x*M, then SAR k)
 *
 *     For some K (where M ≥ 2^N), GCC emits a "fix-up" variant:
 *       t = MULHU(x, M)
 *       q = (t + ((x - t) >> 1)) >> (k-1)
 *
 *   Verification (what we check):
 *     K_recovered = floor(2^(N+k) / M)
 *     The result is correct iff for all x in [0, 2^N-1]:
 *       floor(x * M / 2^(N+k)) == floor(x / K_recovered)
 *     We verify this by checking the Granlund-Montgomery invariant:
 *       M * K >= 2^(N+k)  AND  M * K < 2^(N+k) + 2^k
 *
 * ### Signed 32-bit division by K (GCC/Clang)
 *
 *   M = ceil(2^(N-1+k) / K)    where N=32, K ≥ 2
 *   t = IMULHI(x, M)            (signed high half)
 *   if K > 0: t += x             (for positive K only — the "post-add" form)
 *   q = t >> k                   (arithmetic shift)
 *   q += (q >> 31)               (sign correction: adds 1 if quotient < 0)
 *
 *   Verification:
 *     K_recovered = floor(2^(N-1+k) / |M|)
 *     Check M*K in the correct range for the signed case.
 *
 * ### MSVC 32-bit differences
 *
 *   MSVC sometimes emits a version without the post-add for the positive-M
 *   signed case. We detect both.
 *
 * ### 64-bit variants
 *
 *   Structurally identical with N=64. The multiply produces a 128-bit product;
 *   compilers use MULQ (x86-64), UMULH (AArch64), or __uint128_t tricks.
 *
 * ## Instruction sequence patterns
 *
 * ### Unsigned 32-bit (simple form — MULHU + SHR):
 *   [0]  MOV/MULHI dst0, x, M       ; MULHU(x, M) → dst0
 *   [1]  SHR       dst1, dst0, k    ; >> k
 *
 * ### Unsigned 32-bit (fix-up form — MULHU + SUB + SHR + ADD + SHR):
 *   [0]  MULHI    t0, x, M
 *   [1]  SUB      t1, x, t0         ; x - t0
 *   [2]  SHR      t2, t1, 1         ; >> 1
 *   [3]  ADD      t3, t2, t0        ; t0 + (x-t0)>>1
 *   [4]  SHR      q,  t3, (k-1)     ; >> (k-1)
 *
 * ### Signed 32-bit (GCC/Clang):
 *   [0]  IMULHI   t0, x, M          ; IMULHI(x,M) — high half of signed product
 *   [1]  ADD/SUB  t1, t0, x         ; t0 ± x (the "post-add", optional)
 *   [2]  SAR      t2, t1, k         ; arithmetic right shift
 *   [3]  SHR/SAR  t3, t2, 31        ; sign bit → t3 is 0 or -1
 *   [4]  ADD      q,  t2, t3        ; quotient with sign fix
 *
 * ### Power-of-2 unsigned:
 *   [0]  SHR      q, x, log2(K)
 *
 * ### Power-of-2 signed:
 *   [0]  SAR      t, x, 31           ; sign bit
 *   [1]  SHR      t2, t, (32-k)      ; (or AND with K-1)
 *   [2]  ADD      t3, x, t2
 *   [3]  SAR      q, t3, k
 */

#include <memory>
#include "retdec/idiom_reconstruct/idiom_reconstruct.h"

#include <cassert>
#include <cmath>
#include <cstdint>
#include <optional>
#include <tuple>

namespace retdec {
namespace idiom_reconstruct {

// ─── Magic-number verification ────────────────────────────────────────────────

namespace {

using u64 = uint64_t;

#if defined(_MSC_VER)
// MSVC does not provide __uint128_t; emulate it with MSVC intrinsics.
#include <intrin.h>
struct u128 {
    uint64_t lo = 0, hi = 0;
    u128() = default;
    u128(uint64_t v) : lo(v), hi(0) {}
    u128(uint64_t lo_, uint64_t hi_) : lo(lo_), hi(hi_) {}
    explicit operator uint64_t() const { return lo; }
    explicit operator double() const {
        return (double)hi * 18446744073709551616.0 + (double)lo;
    }
    u128 operator+(const u128& o) const {
        uint64_t r = lo + o.lo;
        return {r, hi + o.hi + (r < lo ? 1u : 0u)};
    }
    u128 operator*(uint64_t b) const {
        uint64_t rhi;
        uint64_t rlo = _umul128(lo, b, &rhi);
        rhi += hi * b;
        return {rlo, rhi};
    }
    u128 operator/(uint64_t b) const {
        if (hi == 0) return {lo / b, 0};
        uint64_t rem;
        return {_udiv128(hi, lo, b, &rem), 0};
    }
    u128 operator<<(int n) const {
        if (n <= 0)   return *this;
        if (n >= 128) return {};
        if (n >= 64)  return {0, lo << (n - 64)};
        return {lo << n, (hi << n) | (lo >> (64 - n))};
    }
    bool operator<(const u128& o)  const { return hi < o.hi || (hi == o.hi && lo < o.lo); }
    bool operator>=(const u128& o) const { return !(*this < o); }
};
// Allow (u128)scalar * scalar
inline u128 operator*(uint64_t a, u128 b)  { return b * a; }
struct s128 {};  // unused placeholder
#else
using u128 = __uint128_t;
using s128 = __int128_t;
#endif

/**
 * Verify the unsigned Granlund-Montgomery invariant.
 *
 * For divisor K, shift k, magic M (N-bit operand width):
 *   M * K ∈ [2^(N+k),  2^(N+k) + 2^k)
 *
 * Returns K if valid, 0 otherwise.
 */
static uint64_t verifyUnsigned(u64 M, int k, int N, CompilerProfile /*prof*/) {
    if (k < 0 || k > 64 || N != 32 && N != 64) return 0;
    if (M == 0) return 0;

    if (N == 32) {
        u128 prod   = (u128)M * M; // placeholder — we need K
        // Recover K = 2^(N+k) / M  (floor)
        if (k + N > 96) return 0;
        u128 twoNk = (u128)1 << (N + k);
        u64 K = (u64)(twoNk / M);
        if (K < 2) return 0;
        // Verify: M * K in [2^(N+k), 2^(N+k) + 2^k)
        u128 mK      = (u128)M * K;
        u128 lo      = twoNk;
        u128 hi      = twoNk + ((u128)1 << k);
        if (mK < lo || mK >= hi) {
            // Try K+1
            u128 mK1 = (u128)M * (K+1);
            if (mK1 >= lo && mK1 < hi) K++;
            else return 0;
        }
        (void)prod;
        return K;
    } else {
        // 64-bit
        if (k + 64 > 127) return 0;
        // For 64-bit we use 128-bit arithmetic
        // 2^(64+k): represented as (hi=2^k, lo=0)
        // M is a 64-bit magic; K = 2^(64+k) / M
        // Approximate: use floating point for initial guess then verify
        if (M == 0) return 0;
        double fM = (double)M;
        // 2^(64+k) = 2^k * 2^64
        // K ≈ 2^k * 2^64 / M = 2^k * (2^64 / M)
        if (k > 63) return 0;
        double twoNk_f = std::ldexp(1.0, 64 + k);
        double K_f     = twoNk_f / fM;
        if (K_f < 2.0 || K_f >= (double)UINT64_MAX) return 0;
        u64 K = (u64)K_f;
        // Verify with 128-bit
        u128 mK  = (u128)M * K;
        // 2^(64+k) = (1<<k) << 64
        u128 lo  = (u128)1 << (64 + k);
        u128 hi  = lo + ((u128)1 << k);
        if (mK < lo || mK >= hi) {
            u128 mK1 = (u128)M * (K+1);
            if (mK1 >= lo && mK1 < hi) K++;
            else return 0;
        }
        return K;
    }
}

/**
 * Verify the signed Granlund-Montgomery invariant.
 *
 * For signed divisor K (> 0), shift k, magic M (N-bit operand width):
 *   M * K ∈ [2^(N-1+k),  2^(N-1+k) + 2^k)
 *
 * Returns K if valid, 0 otherwise.  The sign of K is determined by the
 * sign of M.
 */
static int64_t verifySigned(int64_t M_s, int k, int N, CompilerProfile /*prof*/) {
    if (k < 0 || k > 63) return 0;
    if (N != 32 && N != 64) return 0;
    if (M_s == 0) return 0;

    bool negM = M_s < 0;
    u64  M    = negM ? (u64)(-M_s) : (u64)M_s;

    if (N == 32) {
        // 2^(N-1+k) = 2^(31+k)
        if (31 + k > 96) return 0;
        u128 twoNk = (u128)1 << (31 + k);
        u64 K      = (u64)(twoNk / M);
        if (K < 2) return 0;
        u128 mK    = (u128)M * K;
        u128 lo    = twoNk;
        u128 hi    = twoNk + ((u128)1 << k);
        if (mK < lo || mK >= hi) {
            u128 mK1 = (u128)M * (K+1);
            if (mK1 >= lo && mK1 < hi) K++;
            else return 0;
        }
        return negM ? -(int64_t)K : (int64_t)K;
    } else {
        if (63 + k > 127) return 0;
        u128 twoNk = (u128)1 << (63 + k);
        double K_f = (double)twoNk / (double)M;
        if (K_f < 2.0 || K_f >= (double)UINT64_MAX) return 0;
        u64 K = (u64)K_f;
        u128 mK  = (u128)M * K;
        u128 lo  = twoNk;
        u128 hi  = twoNk + ((u128)1 << k);
        if (mK < lo || mK >= hi) {
            u128 mK1 = (u128)M * (K+1);
            if (mK1 >= lo && mK1 < hi) K++;
            else return 0;
        }
        return negM ? -(int64_t)K : (int64_t)K;
    }
}

/**
 * Recover unsigned divisor K from power-of-2 SHR k.
 * K = 2^k.  Always valid for k in [1,31] (32-bit) or [1,63] (64-bit).
 */
static uint64_t pow2Unsigned(int k, int N) {
    if (k < 1 || k >= N) return 0;
    return (uint64_t)1 << k;
}

/**
 * Check if an instruction is a MULHU/IMULHI (high half of multiply)
 * with one immediate operand (the magic number).
 * Returns (magic, is_signed) or nullopt.
 */
static std::optional<std::pair<int64_t,bool>>
getMagicMul(const IdiomInstr& ins) {
    if (ins.op == IdiomOp::MulHi && ins.isImm(1))
        return std::make_pair(ins.getImm(1), false);
    if (ins.op == IdiomOp::IMulHi && ins.isImm(1))
        return std::make_pair(ins.getImm(1), true);
    // Some compilers emit a regular IMUL that captures high half via CDQ/etc.
    // The platform adapter should have converted those to IMulHi.
    return std::nullopt;
}

// ─── Division matcher ─────────────────────────────────────────────────────────

class DivisionMatcher : public IIdiomMatcher {
public:
    const char* name() const noexcept override { return "DivisionByConstant"; }
    std::size_t minWindowSize() const noexcept override { return 2; }

    std::optional<ReplacementNode> match(const InstrWindow& W,
                                          std::size_t        off,
                                          CompilerProfile    prof) const override
    {
        const std::size_t n = W.size();

        // ── Case 0: Power-of-2 unsigned SHR ───────────────────────────────────
        if (off < n && W[off].op == IdiomOp::Shr && W[off].isImm(1)) {
            int k = (int)W[off].getImm(1);
            int N = (int)W[off].src0.width;
            if (N != 32 && N != 64) goto skip_pow2u;
            if (k >= 1 && k < N) {
                ReplacementNode r;
                r.kind        = ReplacementKind::DivUnsigned;
                r.inputReg    = W[off].src0.reg;
                r.outputReg   = W[off].dst.reg;
                r.divisor     = (int64_t)pow2Unsigned(k, N);
                r.operandWidth= N;
                r.firstVma    = W[off].vma;
                r.lastVma     = W[off].vma;
                r.instrCount  = 1;
                return r;
            }
        }
        skip_pow2u:;

        // ── Case 1: Power-of-2 signed SAR ─────────────────────────────────────
        // Pattern: SAR t, x, 31  (+offset); SAR/ADD chain; SAR q, t3, k
        // We look for: SAR x, k at position [off] where [off-1] (not in window)
        // but more robustly: SAR q, x, k  with N-1 ≥ k ≥ 1 (may have sign-fix
        // before).  We detect the simplest case where only the two-instruction
        // sign-fix + SAR is present.
        if (off+1 < n &&
            W[off].op == IdiomOp::Sar && W[off].isImm(1) &&
            W[off+1].op == IdiomOp::Shr)
        {
            // Pattern: SAR t, x, 31  then SHR t2, t, (N-k)  then ADD — skip, too complex
            // Fall through to full pattern below.
        }

        // ── Case 2: MULHI + SHR (simple unsigned) ─────────────────────────────
        if (off+1 < n) {
            auto mg = getMagicMul(W[off]);
            if (mg && !mg->second) {  // unsigned
                uint64_t M = (uint64_t)mg->first;
                // Next instruction: SHR dst, mulhi_result, k
                if (W[off+1].op == IdiomOp::Shr && W[off+1].isImm(1) &&
                    W[off+1].src0.reg == W[off].dst.reg)
                {
                    int k = (int)W[off+1].getImm(1);
                    int N = (int)W[off].src0.width;
                    if (N != 32 && N != 64) goto skip_simple_u;
                    uint64_t K = verifyUnsigned(M, k, N, prof);
                    if (K >= 2) {
                        ReplacementNode r;
                        r.kind        = ReplacementKind::DivUnsigned;
                        r.inputReg    = W[off].src0.reg;
                        r.outputReg   = W[off+1].dst.reg;
                        r.divisor     = (int64_t)K;
                        r.operandWidth= (uint32_t)N;
                        r.firstVma    = W[off].vma;
                        r.lastVma     = W[off+1].vma;
                        r.instrCount  = 2;
                        return r;
                    }
                }
                // Also accept: SHR with k==0 means just MULHI (k=0 variant)
                if (W[off+1].op == IdiomOp::Shr && W[off+1].isImm(1) &&
                    W[off+1].getImm(1) == 0 &&
                    W[off+1].src0.reg == W[off].dst.reg)
                {
                    int N = (int)W[off].src0.width;
                    uint64_t K = verifyUnsigned(M, 0, N, prof);
                    if (K >= 2) {
                        ReplacementNode r;
                        r.kind        = ReplacementKind::DivUnsigned;
                        r.inputReg    = W[off].src0.reg;
                        r.outputReg   = W[off+1].dst.reg;
                        r.divisor     = (int64_t)K;
                        r.operandWidth= (uint32_t)N;
                        r.firstVma    = W[off].vma;
                        r.lastVma     = W[off+1].vma;
                        r.instrCount  = 2;
                        return r;
                    }
                }
            }
        }
        skip_simple_u:;

        // ── Case 3: MULHI + SUB + SHR + ADD + SHR (fix-up unsigned) ──────────
        if (off+4 < n) {
            auto mg = getMagicMul(W[off]);
            if (mg && !mg->second) {
                uint64_t M = (uint64_t)mg->first;
                uint32_t mulDst = W[off].dst.reg;
                uint32_t xReg   = W[off].src0.reg;
                // [1] SUB t1, x, mulDst
                if (W[off+1].op==IdiomOp::Sub &&
                    W[off+1].src0.reg==xReg &&
                    W[off+1].src1.reg==mulDst)
                {
                    uint32_t subDst = W[off+1].dst.reg;
                    // [2] SHR t2, subDst, 1
                    if (W[off+2].op==IdiomOp::Shr &&
                        W[off+2].isImm(1) && W[off+2].getImm(1)==1 &&
                        W[off+2].src0.reg==subDst)
                    {
                        uint32_t shrDst = W[off+2].dst.reg;
                        // [3] ADD t3, shrDst, mulDst
                        if (W[off+3].op==IdiomOp::Add &&
                            ((W[off+3].src0.reg==shrDst && W[off+3].src1.reg==mulDst) ||
                             (W[off+3].src0.reg==mulDst && W[off+3].src1.reg==shrDst)))
                        {
                            uint32_t addDst = W[off+3].dst.reg;
                            // [4] SHR q, addDst, k-1
                            if (W[off+4].op==IdiomOp::Shr && W[off+4].isImm(1) &&
                                W[off+4].src0.reg==addDst)
                            {
                                int km1 = (int)W[off+4].getImm(1);
                                int k   = km1 + 1;
                                int N   = (int)W[off].src0.width;
                                if (N==32 || N==64) {
                                    uint64_t K = verifyUnsigned(M, k, N, prof);
                                    if (K >= 2) {
                                        ReplacementNode r;
                                        r.kind        = ReplacementKind::DivUnsigned;
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
        }

        // ── Case 4: IMULHI + (optional ADD x) + SAR k + SAR 31 + ADD ─────────
        //   Signed division pattern (GCC/Clang/MSVC).
        if (off+3 < n) {
            auto mg = getMagicMul(W[off]);
            if (mg && mg->second) {   // signed
                int64_t  M_s    = mg->first;
                uint32_t mulDst = W[off].dst.reg;
                uint32_t xReg   = W[off].src0.reg;
                int N = (int)W[off].src0.width;

                std::size_t nextIdx = off+1;
                bool hadPostAdd = false;
                // Optional post-add: ADD t, mulDst, x  (when M positive)
                if (nextIdx < n &&
                    W[nextIdx].op==IdiomOp::Add &&
                    ((W[nextIdx].src0.reg==mulDst && W[nextIdx].src1.reg==xReg) ||
                     (W[nextIdx].src0.reg==xReg   && W[nextIdx].src1.reg==mulDst)))
                {
                    mulDst   = W[nextIdx].dst.reg;
                    hadPostAdd= true;
                    ++nextIdx;
                }
                // Optional: SUB t, mulDst, x (for negative M post-sub form)
                if (!hadPostAdd && nextIdx < n &&
                    W[nextIdx].op==IdiomOp::Sub &&
                    W[nextIdx].src0.reg==mulDst && W[nextIdx].src1.reg==xReg)
                {
                    mulDst   = W[nextIdx].dst.reg;
                    hadPostAdd= true;
                    ++nextIdx;
                }

                // SAR t2, mulDst, k
                if (nextIdx < n &&
                    W[nextIdx].op==IdiomOp::Sar && W[nextIdx].isImm(1) &&
                    W[nextIdx].src0.reg==mulDst)
                {
                    int      k      = (int)W[nextIdx].getImm(1);
                    uint32_t sarDst = W[nextIdx].dst.reg;
                    ++nextIdx;
                    // SHR/SAR sign: [nextIdx] SHR t3, sarDst, N-1
                    if (nextIdx < n &&
                        (W[nextIdx].op==IdiomOp::Shr || W[nextIdx].op==IdiomOp::Sar) &&
                        W[nextIdx].isImm(1) && W[nextIdx].getImm(1)==(N-1) &&
                        W[nextIdx].src0.reg==sarDst)
                    {
                        uint32_t signDst = W[nextIdx].dst.reg;
                        ++nextIdx;
                        // ADD q, sarDst, signDst
                        if (nextIdx < n &&
                            W[nextIdx].op==IdiomOp::Add &&
                            ((W[nextIdx].src0.reg==sarDst && W[nextIdx].src1.reg==signDst) ||
                             (W[nextIdx].src0.reg==signDst && W[nextIdx].src1.reg==sarDst)))
                        {
                            if (N==32 || N==64) {
                                int64_t K = verifySigned(M_s, k, N, prof);
                                if (K != 0) {
                                    ReplacementNode r;
                                    r.kind        = ReplacementKind::DivSigned;
                                    r.inputReg    = xReg;
                                    r.outputReg   = W[nextIdx].dst.reg;
                                    r.divisor     = K;
                                    r.operandWidth= (uint32_t)N;
                                    r.firstVma    = W[off].vma;
                                    r.lastVma     = W[nextIdx].vma;
                                    r.instrCount  = nextIdx - off + 1;
                                    return r;
                                }
                            }
                        }
                    }
                }
            }
        }

        // ── Case 5: Signed power-of-2 (SAR + sign-fix) ───────────────────────
        // Pattern (GCC): SAR t, x, 31; SHR t2, t, (N-k); ADD t3, x, t2; SAR q, t3, k
        if (off+3 < n &&
            W[off].op==IdiomOp::Sar && W[off].isImm(1) &&
            W[off].getImm(1)==31)
        {
            uint32_t xReg   = W[off].src0.reg;
            uint32_t t0     = W[off].dst.reg;
            // SHR t2, t0, (32-k)
            if (W[off+1].op==IdiomOp::Shr && W[off+1].isImm(1) && W[off+1].src0.reg==t0) {
                int shift2 = (int)W[off+1].getImm(1);
                int k = 32 - shift2; // k = 32 - (32-k)
                if (k >= 1 && k <= 30) {
                    uint32_t t2 = W[off+1].dst.reg;
                    // ADD t3, x, t2
                    if (W[off+2].op==IdiomOp::Add &&
                        ((W[off+2].src0.reg==xReg && W[off+2].src1.reg==t2) ||
                         (W[off+2].src0.reg==t2   && W[off+2].src1.reg==xReg)))
                    {
                        uint32_t t3 = W[off+2].dst.reg;
                        // SAR q, t3, k
                        if (W[off+3].op==IdiomOp::Sar && W[off+3].isImm(1) &&
                            W[off+3].getImm(1)==k && W[off+3].src0.reg==t3)
                        {
                            int64_t K = (int64_t)1 << k;
                            ReplacementNode r;
                            r.kind        = ReplacementKind::DivSigned;
                            r.inputReg    = xReg;
                            r.outputReg   = W[off+3].dst.reg;
                            r.divisor     = K;
                            r.operandWidth= 32;
                            r.firstVma    = W[off].vma;
                            r.lastVma     = W[off+3].vma;
                            r.instrCount  = 4;
                            return r;
                        }
                    }
                }
            }
        }

        return std::nullopt;
    }
};

} // anon namespace

std::unique_ptr<IIdiomMatcher> makeDivisionMatcher() {
    return std::make_unique<DivisionMatcher>();
}

} // namespace idiom_reconstruct
} // namespace retdec
