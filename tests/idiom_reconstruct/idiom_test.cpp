/**
 * @file tests/idiom_reconstruct/idiom_test.cpp
 * @brief Comprehensive tests for the semantic idiom reconstruction engine.
 *
 * ## Test strategy
 *
 * ### Division corpus
 * We generate the actual magic numbers that GCC/Clang/MSVC emit for each K in
 * [2..100] using the Granlund-Montgomery algorithm, then construct the exact
 * IdiomInstr sequence the compiler would emit, and assert the matcher recovers
 * K correctly.
 *
 * ### Other idioms
 * We construct synthetic instruction windows that exactly match each pattern
 * and assert the replacement node has the correct kind and operand values.
 */

#include "retdec/idiom_reconstruct/idiom_reconstruct.h"
#include <gtest/gtest.h>
#include <cstdint>

#if defined(_MSC_VER)
#include <intrin.h>
struct u128 {
    uint64_t lo = 0, hi = 0;
    u128() = default;
    u128(uint64_t v) : lo(v), hi(0) {}
    u128(uint64_t lo_, uint64_t hi_) : lo(lo_), hi(hi_) {}
    explicit operator uint64_t() const { return lo; }
    explicit operator bool()     const { return lo || hi; }
    bool operator<(const u128& o)  const { return hi < o.hi || (hi == o.hi && lo < o.lo); }
    bool operator>=(const u128& o) const { return !(*this < o); }
    bool operator==(const u128& o) const { return lo == o.lo && hi == o.hi; }
    bool operator!=(const u128& o) const { return !(*this == o); }
    u128 operator+(const u128& o) const {
        uint64_t r = lo + o.lo;
        return {r, hi + o.hi + (r < lo ? 1u : 0u)};
    }
    u128 operator-(const u128& o) const {
        return {lo - o.lo, hi - o.hi - (lo < o.lo ? 1u : 0u)};
    }
    u128 operator*(uint64_t b) const {
        uint64_t rhi; uint64_t rlo = _umul128(lo, b, &rhi); rhi += hi * b;
        return {rlo, rhi};
    }
    u128 operator/(uint64_t b) const {
        if (hi == 0) return {lo / b, 0};
        uint64_t rem; return {_udiv128(hi, lo, b, &rem), 0};
    }
    u128 operator%(uint64_t b) const {
        if (hi == 0) return {lo % b, 0};
        uint64_t rem; _udiv128(hi, lo, b, &rem); return {rem, 0};
    }
    u128 operator<<(int n) const {
        if (n <= 0) return *this; if (n >= 128) return {};
        if (n >= 64) return {0, lo << (n - 64)};
        return {lo << n, (hi << n) | (lo >> (64 - n))};
    }
    u128 operator>>(int n) const {
        if (n <= 0) return *this; if (n >= 128) return {};
        if (n >= 64) return {hi >> (n - 64), 0};
        return {(lo >> n) | (hi << (64 - n)), hi >> n};
    }
};
// Free operator helpers to support mixed u128/u64 arithmetic
inline u128 operator+(u128 a, uint64_t b) { return a + u128(b); }
inline u128 operator+(uint64_t a, u128 b) { return u128(a) + b; }
inline u128 operator-(u128 a, uint64_t b) { return a - u128(b); }
inline u128 operator*(uint64_t a, u128 b) { return b * a; }
#else
using u128 = __uint128_t;
#endif
#include <tuple>
#include <vector>

using namespace retdec::idiom_reconstruct;

// ─── Instruction builder helpers ─────────────────────────────────────────────

// Unique virtual register allocator
static uint32_t nextReg() {
    static uint32_t ctr = 1;
    return ctr++;
}

static IdiomInstr make(IdiomOp op, uint32_t dst, uint32_t s0,
                        int64_t imm, uint32_t width=32) {
    IdiomInstr ins;
    ins.op       = op;
    ins.dst      = IdiomOperand::reg32(dst);
    ins.src0     = IdiomOperand::reg32(s0);
    ins.src1     = IdiomOperand::makeImm(imm, width);
    ins.dst.width= width;
    ins.src0.width=width;
    return ins;
}
static IdiomInstr makeRR(IdiomOp op, uint32_t dst, uint32_t s0,
                           uint32_t s1, uint32_t width=32) {
    IdiomInstr ins;
    ins.op       = op;
    ins.dst      = IdiomOperand::reg32(dst);
    ins.dst.width= width;
    ins.src0     = IdiomOperand::reg32(s0);
    ins.src0.width=width;
    ins.src1     = IdiomOperand::reg32(s1);
    ins.src1.width=width;
    return ins;
}
static IdiomInstr makeMulHi(bool isSigned, uint32_t dst, uint32_t x, int64_t M, uint32_t w=32) {
    IdiomInstr ins;
    ins.op       = isSigned ? IdiomOp::IMulHi : IdiomOp::MulHi;
    ins.dst      = IdiomOperand::reg32(dst); ins.dst.width=w;
    ins.src0     = IdiomOperand::reg32(x);   ins.src0.width=w;
    ins.src1     = IdiomOperand::makeImm(M, w);
    return ins;
}

// ─── Magic number computation (mirrors the algorithm in division_recovery.cpp) ─

#if !defined(_MSC_VER)
using u128 = __uint128_t;
#endif

struct MagicU32 {
    uint64_t M;  // magic multiplier
    int      k;  // shift amount
    bool     fixup; // true if fix-up form needed
};

/// Compute GCC/Clang unsigned 32-bit magic number for divisor K.
static MagicU32 magicUnsigned32(uint64_t K) {
    // Find smallest k such that M = ceil(2^(32+k)/K) fits in 32 bits
    for (int k = 0; k <= 31; ++k) {
        u128 twoNk = (u128)1 << (32 + k);
        u128 M     = (twoNk + K - 1) / K;  // ceil
        if (M < ((u128)1 << 32)) {
            // Check if fix-up is needed: M >= 2^32 in the unconstrained form
            u128 M0 = (twoNk + K - 1) / K;
            bool fixup = (M0 >= ((u128)1 << 32));
            return { (uint64_t)M, k, fixup };
        }
        // M >= 2^32: use fix-up form (emit at k=k-1 with extra instructions)
        // The fix-up form uses M' = M - 2^32 which fits, and emits additional
        // ADD/SHR instructions.
        if (k > 0) {
            u128 Mprev = (((u128)1 << (32+k-1)) + K - 1) / K;
            if (Mprev >= ((u128)1 << 32)) {
                // Still doesn't fit: use fix-up
                u128 Mfix = M - ((u128)1 << 32);
                if (Mfix < ((u128)1 << 32)) {
                    return { (uint64_t)Mfix, k, true };
                }
            }
        }
    }
    return { 0, 0, false }; // fallback
}

struct MagicS32 {
    int64_t M;   // signed magic (may be negative for negative K)
    int     k;   // shift amount
    bool    postAdd; // true if post-add of x is needed
};

/// Compute GCC/Clang signed 32-bit magic number for divisor K (K > 0).
static MagicS32 magicSigned32(int64_t K) {
    assert(K >= 2);
    for (int k = 0; k <= 30; ++k) {
        u128 twoNk = (u128)1 << (31 + k);
        u128 M     = (twoNk + (uint64_t)K - 1) / (uint64_t)K;
        if (M < ((u128)1 << 31)) {
            // Positive M, no post-add needed when (M*K) in correct range
            // Check M*K in [2^(31+k), 2^(31+k) + 2^k)
            u128 mK  = M * (uint64_t)K;
            u128 lo  = twoNk;
            u128 hi  = twoNk + ((u128)1 << k);
            if (mK >= lo && mK < hi) {
                return { (int64_t)(uint64_t)M, k, false };
            }
            // Post-add needed: M' = M - 2^31; then after IMULHI add x
            int64_t Mfix = (int64_t)(uint64_t)(M - ((u128)1<<31));
            return { Mfix, k, true };
        }
        // M >= 2^31: treat as negative (wrap-around)
        int64_t Mneg = (int64_t)(uint64_t)(M - ((u128)1<<32));
        if (Mneg < 0) {
            // Negative magic for positive K requires post-sub (sub x from IMULHI result)
            // Verification: (Mneg+2^32)*K in [2^(31+k), 2^(31+k)+2^k)
            u128 absM  = (u128)(-Mneg);
            u128 effM  = ((u128)1<<32) - absM;  // M in positive form
            u128 mK    = effM * (uint64_t)K;
            u128 lo    = twoNk;
            u128 hi    = twoNk + ((u128)1 << k);
            if (mK >= lo && mK < hi) {
                return { Mneg, k, true }; // post-sub of x
            }
        }
    }
    return { 0, 0, false };
}

// ─── Build division instruction windows ──────────────────────────────────────

/// Build the "simple" unsigned division window: MULHU x, M; SHR q, t, k
static InstrWindow buildUnsignedSimple(uint32_t xReg, uint32_t& qReg,
                                        uint64_t M, int k, uint32_t w=32) {
    uint32_t t = nextReg();
    qReg       = nextReg();
    InstrWindow w2;
    w2.push_back(makeMulHi(false, t, xReg, (int64_t)M, w));
    w2.push_back(make(IdiomOp::Shr, qReg, t, k, w));
    return w2;
}

/// Build the "fix-up" unsigned division window (5 instructions)
static InstrWindow buildUnsignedFixup(uint32_t xReg, uint32_t& qReg,
                                       uint64_t M, int k, uint32_t w=32) {
    uint32_t t0=nextReg(), t1=nextReg(), t2=nextReg(), t3=nextReg();
    qReg=nextReg();
    InstrWindow r;
    r.push_back(makeMulHi(false, t0, xReg, (int64_t)M, w));
    r.push_back(makeRR(IdiomOp::Sub, t1, xReg, t0, w));
    r.push_back(make(IdiomOp::Shr, t2, t1, 1, w));
    r.push_back(makeRR(IdiomOp::Add, t3, t2, t0, w));
    r.push_back(make(IdiomOp::Shr, qReg, t3, k-1, w));
    return r;
}

/// Build the signed division window (4-5 instructions)
static InstrWindow buildSigned(uint32_t xReg, uint32_t& qReg,
                                int64_t M, int k, bool postAdd, uint32_t w=32) {
    uint32_t t0=nextReg(), t1=nextReg(), t2=nextReg(), t3=nextReg();
    qReg=nextReg();
    InstrWindow r;
    r.push_back(makeMulHi(true, t0, xReg, M, w));
    if (postAdd) {
        // Add xReg to t0
        uint32_t addDst=nextReg();
        r.push_back(makeRR(IdiomOp::Add, addDst, t0, xReg, w));
        t0=addDst;
    }
    r.push_back(make(IdiomOp::Sar, t1, t0, k, w));
    r.push_back(make(IdiomOp::Shr, t2, t1, w-1, w));  // sign bit
    r.push_back(makeRR(IdiomOp::Add, qReg, t1, t2, w));
    return r;
}

// ─── Division tests (unsigned, K in [2..100]) ─────────────────────────────────

class DivisionUnsignedTest : public ::testing::TestWithParam<uint64_t> {};

TEST_P(DivisionUnsignedTest, RecoversDivisorCorrectly) {
    uint64_t K = GetParam();
    auto magic = magicUnsigned32(K);
    ASSERT_NE(magic.M, 0u) << "Magic computation failed for K=" << K;

    uint32_t x = nextReg(), q = 0;
    InstrWindow window;
    if (magic.fixup) {
        window = buildUnsignedFixup(x, q, magic.M, magic.k);
    } else {
        window = buildUnsignedSimple(x, q, magic.M, magic.k);
    }

    auto engine = makeDefaultEngine(CompilerProfile::GCC);
    auto results = engine.process(window);

    ASSERT_FALSE(results.empty()) << "No idiom matched for K=" << K
        << " M=0x" << std::hex << magic.M << " k=" << std::dec << magic.k
        << " fixup=" << magic.fixup;
    EXPECT_EQ(results[0].kind, ReplacementKind::DivUnsigned) << "K=" << K;
    EXPECT_EQ((uint64_t)results[0].divisor, K)
        << "Wrong divisor for K=" << K << " got=" << results[0].divisor
        << " M=" << magic.M << " k=" << magic.k;
}

// Generate K in [2..100]
static std::vector<uint64_t> unsignedDivisors() {
    std::vector<uint64_t> v;
    for (uint64_t k=2; k<=100; ++k) v.push_back(k);
    // Also add some larger interesting values
    for (uint64_t k : {101u, 127u, 128u, 255u, 256u, 1000u, 65535u, 65536u})
        v.push_back(k);
    return v;
}

INSTANTIATE_TEST_SUITE_P(Unsigned32, DivisionUnsignedTest,
    ::testing::ValuesIn(unsignedDivisors()),
    [](const ::testing::TestParamInfo<uint64_t>& i){
        return "K" + std::to_string(i.param);
    });

// ─── Division tests (signed, K in [2..100]) ───────────────────────────────────

class DivisionSignedTest : public ::testing::TestWithParam<int64_t> {};

TEST_P(DivisionSignedTest, RecoversDivisorCorrectly) {
    int64_t K = GetParam();
    auto magic = magicSigned32(K);
    ASSERT_NE(magic.M, 0) << "Magic computation failed for K=" << K;

    uint32_t x = nextReg(), q = 0;
    auto window = buildSigned(x, q, magic.M, magic.k, magic.postAdd);

    auto engine = makeDefaultEngine(CompilerProfile::GCC);
    auto results = engine.process(window);

    ASSERT_FALSE(results.empty()) << "No idiom matched for K=" << K
        << " M=" << magic.M << " k=" << magic.k;
    EXPECT_EQ(results[0].kind, ReplacementKind::DivSigned) << "K=" << K;
    EXPECT_EQ(results[0].divisor, K)
        << "Wrong divisor for K=" << K << " got=" << results[0].divisor;
}

static std::vector<int64_t> signedDivisors() {
    std::vector<int64_t> v;
    for (int64_t k=2; k<=100; ++k) v.push_back(k);
    return v;
}

INSTANTIATE_TEST_SUITE_P(Signed32, DivisionSignedTest,
    ::testing::ValuesIn(signedDivisors()),
    [](const ::testing::TestParamInfo<int64_t>& i){
        return "K" + std::to_string(i.param);
    });

// ─── Power-of-2 unsigned division ────────────────────────────────────────────

TEST(DivisionPow2Test, ShrByOne) {
    uint32_t x=nextReg(), q=nextReg();
    InstrWindow w = { make(IdiomOp::Shr, q, x, 1) };
    auto e = makeDefaultEngine();
    auto r = e.process(w);
    ASSERT_EQ(r.size(), 1u);
    EXPECT_EQ(r[0].kind, ReplacementKind::DivUnsigned);
    EXPECT_EQ(r[0].divisor, 2);
}

TEST(DivisionPow2Test, ShrByFour) {
    uint32_t x=nextReg(), q=nextReg();
    InstrWindow w = { make(IdiomOp::Shr, q, x, 4) };
    auto e = makeDefaultEngine();
    auto r = e.process(w);
    ASSERT_EQ(r.size(), 1u);
    EXPECT_EQ(r[0].divisor, 16);
}

TEST(DivisionPow2Test, Shr31IsMaxPow2) {
    uint32_t x=nextReg(), q=nextReg();
    InstrWindow w = { make(IdiomOp::Shr, q, x, 31) };
    auto e = makeDefaultEngine();
    auto r = e.process(w);
    ASSERT_EQ(r.size(), 1u);
    EXPECT_EQ((uint64_t)r[0].divisor, (uint64_t)1<<31);
}

// ─── Power-of-2 signed division (SAR sign-fix form) ──────────────────────────

TEST(DivisionSignedPow2Test, DivBy4) {
    // SAR t0, x, 31; SHR t1, t0, 30; ADD t2, x, t1; SAR q, t2, 2
    uint32_t x=nextReg(), t0=nextReg(), t1=nextReg(), t2=nextReg(), q=nextReg();
    InstrWindow w = {
        make(IdiomOp::Sar, t0, x,  31),  // sign bit
        make(IdiomOp::Shr, t1, t0, 30),  // 32-2 = 30
        makeRR(IdiomOp::Add, t2, x, t1),
        make(IdiomOp::Sar, q,  t2, 2),
    };
    auto e = makeDefaultEngine();
    auto r = e.process(w);
    ASSERT_EQ(r.size(), 1u);
    EXPECT_EQ(r[0].kind, ReplacementKind::DivSigned);
    EXPECT_EQ(r[0].divisor, 4);
}

TEST(DivisionSignedPow2Test, DivBy8) {
    uint32_t x=nextReg(), t0=nextReg(), t1=nextReg(), t2=nextReg(), q=nextReg();
    InstrWindow w = {
        make(IdiomOp::Sar, t0, x,  31),
        make(IdiomOp::Shr, t1, t0, 29),  // 32-3=29
        makeRR(IdiomOp::Add, t2, x, t1),
        make(IdiomOp::Sar, q,  t2, 3),
    };
    auto e = makeDefaultEngine();
    auto r = e.process(w);
    ASSERT_EQ(r.size(), 1u);
    EXPECT_EQ(r[0].divisor, 8);
}

// ─── Modulo tests ─────────────────────────────────────────────────────────────

TEST(ModuloTest, UnsignedPow2By4) {
    uint32_t x=nextReg(), r_=nextReg();
    InstrWindow w = { make(IdiomOp::And, r_, x, 3) }; // x & (4-1)
    auto e = makeDefaultEngine();
    auto r = e.process(w);
    ASSERT_EQ(r.size(), 1u);
    EXPECT_EQ(r[0].kind, ReplacementKind::ModUnsigned);
    EXPECT_EQ(r[0].divisor, 4);
}

TEST(ModuloTest, UnsignedPow2By16) {
    uint32_t x=nextReg(), r_=nextReg();
    InstrWindow w = { make(IdiomOp::And, r_, x, 15) };
    auto e = makeDefaultEngine();
    auto r = e.process(w);
    ASSERT_EQ(r.size(), 1u);
    EXPECT_EQ(r[0].divisor, 16);
}

TEST(ModuloTest, GeneralModuloSuffix) {
    // MUL t, q, 7; SUB r, x, t
    uint32_t q=nextReg(), t=nextReg(), x=nextReg(), r_=nextReg();
    InstrWindow w = {
        make(IdiomOp::IMul, t, q, 7),
        makeRR(IdiomOp::Sub, r_, x, t),
    };
    auto e = makeDefaultEngine();
    auto r = e.process(w);
    ASSERT_EQ(r.size(), 1u);
    EXPECT_EQ(r[0].kind, ReplacementKind::ModUnsigned);
    EXPECT_EQ(r[0].divisor, 7);
}

TEST(ModuloTest, SignedPow2Mod4) {
    // SAR t0, x, 31; SHR t1, t0, 30; ADD t2, x, t1; AND t3, t2, -4; SUB r, x, t3
    uint32_t x=nextReg(), t0=nextReg(), t1=nextReg(), t2=nextReg(), t3=nextReg(), r_=nextReg();
    InstrWindow w = {
        make(IdiomOp::Sar, t0, x,  31),
        make(IdiomOp::Shr, t1, t0, 30),
        makeRR(IdiomOp::Add, t2, x, t1),
        make(IdiomOp::And, t3, t2, (int64_t)-4),
        makeRR(IdiomOp::Sub, r_, x, t3),
    };
    auto e = makeDefaultEngine();
    auto r = e.process(w);
    ASSERT_EQ(r.size(), 1u);
    EXPECT_EQ(r[0].kind, ReplacementKind::ModSigned);
    EXPECT_EQ(r[0].divisor, 4);
}

// ─── Abs value tests ──────────────────────────────────────────────────────────

TEST(AbsTest, FormA_CDQ) {
    uint32_t eax=nextReg(), edx=nextReg();
    IdiomInstr cdq; cdq.op=IdiomOp::Cdq; cdq.dst=IdiomOperand::reg32(edx); cdq.src0=IdiomOperand::reg32(eax);
    InstrWindow w = {
        cdq,
        makeRR(IdiomOp::Xor, eax, eax, edx),
        makeRR(IdiomOp::Sub, eax, eax, edx),
    };
    auto e = makeDefaultEngine();
    auto r = e.process(w);
    ASSERT_EQ(r.size(), 1u);
    EXPECT_EQ(r[0].kind, ReplacementKind::AbsValue);
    EXPECT_EQ(r[0].inputReg, eax);
}

TEST(AbsTest, FormB_SAR_XOR_SUB) {
    uint32_t x=nextReg(), t=nextReg(), t2=nextReg(), q=nextReg();
    InstrWindow w = {
        make(IdiomOp::Sar, t,  x,  31),
        makeRR(IdiomOp::Xor, t2, x, t),
        makeRR(IdiomOp::Sub, q,  t2, t),
    };
    auto e = makeDefaultEngine();
    auto r = e.process(w);
    ASSERT_EQ(r.size(), 1u);
    EXPECT_EQ(r[0].kind, ReplacementKind::AbsValue);
}

TEST(AbsTest, FormB_SAR_ADD_XOR) {
    // ARM/RISC-V variant: SAR t, x, 31; ADD t2, x, t; XOR r, t2, t
    uint32_t x=nextReg(), t=nextReg(), t2=nextReg(), r_=nextReg();
    InstrWindow w = {
        make(IdiomOp::Sar, t,  x, 31),
        makeRR(IdiomOp::Add, t2, x, t),
        makeRR(IdiomOp::Xor, r_,  t2, t),
    };
    auto e = makeDefaultEngine();
    auto r = e.process(w);
    ASSERT_EQ(r.size(), 1u);
    EXPECT_EQ(r[0].kind, ReplacementKind::AbsValue);
}

TEST(AbsTest, FormD_MOVSX_SAR_XOR_SUB) {
    uint32_t x=nextReg(), ext=nextReg(), t=nextReg(), t2=nextReg(), r_=nextReg();
    IdiomInstr movsx;
    movsx.op=IdiomOp::Movsx; movsx.dst=IdiomOperand::reg64(ext); movsx.src0=IdiomOperand::reg32(x);
    InstrWindow w = {
        movsx,
        make(IdiomOp::Sar, t,  ext, 63, 64),
        makeRR(IdiomOp::Xor, t2, x, t, 64),
        makeRR(IdiomOp::Sub, r_, t2, t, 64),
    };
    auto e = makeDefaultEngine();
    auto r = e.process(w);
    ASSERT_EQ(r.size(), 1u);
    EXPECT_EQ(r[0].kind, ReplacementKind::AbsValue);
}

// ─── Bit manipulation tests ───────────────────────────────────────────────────

TEST(BitTest, LowestSetBit) {
    uint32_t x=nextReg(), t=nextReg(), r_=nextReg();
    IdiomInstr neg; neg.op=IdiomOp::Neg; neg.dst=IdiomOperand::reg32(t); neg.src0=IdiomOperand::reg32(x);
    InstrWindow w = { neg, makeRR(IdiomOp::And, r_, x, t) };
    auto e = makeDefaultEngine();
    auto r = e.process(w);
    ASSERT_EQ(r.size(), 1u);
    EXPECT_EQ(r[0].kind, ReplacementKind::LowestSetBit);
}

TEST(BitTest, ClearLowestSetBit_Sub) {
    uint32_t x=nextReg(), t=nextReg(), r_=nextReg();
    InstrWindow w = {
        make(IdiomOp::Sub, t,  x, 1),
        makeRR(IdiomOp::And, r_, x, t),
    };
    auto e = makeDefaultEngine();
    auto r = e.process(w);
    ASSERT_EQ(r.size(), 1u);
    EXPECT_EQ(r[0].kind, ReplacementKind::ClearLowestSetBit);
}

TEST(BitTest, ClearLowestSetBit_AddNeg1) {
    uint32_t x=nextReg(), t=nextReg(), r_=nextReg();
    InstrWindow w = {
        make(IdiomOp::Add, t,  x, -1),
        makeRR(IdiomOp::And, r_, x, t),
    };
    auto e = makeDefaultEngine();
    auto r = e.process(w);
    ASSERT_EQ(r.size(), 1u);
    EXPECT_EQ(r[0].kind, ReplacementKind::ClearLowestSetBit);
}

TEST(BitTest, IsZero_NOT_AND1) {
    uint32_t x=nextReg(), t=nextReg(), r_=nextReg();
    IdiomInstr not_; not_.op=IdiomOp::Not; not_.dst=IdiomOperand::reg32(t); not_.src0=IdiomOperand::reg32(x);
    InstrWindow w = { not_, make(IdiomOp::And, r_, t, 1) };
    auto e = makeDefaultEngine();
    auto r = e.process(w);
    ASSERT_EQ(r.size(), 1u);
    EXPECT_EQ(r[0].kind, ReplacementKind::IsZero);
}

TEST(BitTest, ToBool_NEG_SBB_NEG) {
    uint32_t x=nextReg(), t=nextReg(), sb=nextReg(), r_=nextReg();
    IdiomInstr neg1, neg2, sbb;
    neg1.op=IdiomOp::Neg; neg1.dst=IdiomOperand::reg32(t);  neg1.src0=IdiomOperand::reg32(x);
    sbb.op =IdiomOp::Sub; sbb.dst =IdiomOperand::reg32(sb); sbb.src0=IdiomOperand::reg32(sb); sbb.src1=IdiomOperand::reg32(sb);
    neg2.op=IdiomOp::Neg; neg2.dst=IdiomOperand::reg32(r_); neg2.src0=IdiomOperand::reg32(sb);
    InstrWindow w = { neg1, sbb, neg2 };
    auto e = makeDefaultEngine();
    auto r = e.process(w);
    ASSERT_EQ(r.size(), 1u);
    EXPECT_EQ(r[0].kind, ReplacementKind::ToBool);
}

TEST(BitTest, Popcount32) {
    // Build the full 12-instruction parallel bit-count sequence
    uint32_t x=nextReg(),t0,t1,t2,t3,t4,t5,t6,t7,t8,t9,t10,q;
    t0=nextReg();t1=nextReg();t2=nextReg();t3=nextReg();t4=nextReg();
    t5=nextReg();t6=nextReg();t7=nextReg();t8=nextReg();t9=nextReg();
    t10=nextReg();q=nextReg();
    InstrWindow w = {
        make(IdiomOp::Shr, t0,  x,   1),
        make(IdiomOp::And, t1,  t0,  (int64_t)0x55555555),
        makeRR(IdiomOp::Sub,   t2, x,  t1),
        make(IdiomOp::And, t3,  t2,  (int64_t)0x33333333),
        make(IdiomOp::Shr, t4,  t2,  2),
        make(IdiomOp::And, t5,  t4,  (int64_t)0x33333333),
        makeRR(IdiomOp::Add,   t6, t3, t5),
        make(IdiomOp::Shr, t7,  t6,  4),
        makeRR(IdiomOp::Add,   t8, t6, t7),
        make(IdiomOp::And, t9,  t8,  (int64_t)0x0F0F0F0F),
        make(IdiomOp::Mul, t10, t9,  (int64_t)0x01010101),
        make(IdiomOp::Shr, q,   t10, 24),
    };
    auto e = makeDefaultEngine();
    auto r = e.process(w);
    ASSERT_EQ(r.size(), 1u);
    EXPECT_EQ(r[0].kind, ReplacementKind::Popcount);
    EXPECT_EQ(r[0].inputReg, x);
    EXPECT_EQ(r[0].instrCount, 12u);
}

TEST(BitTest, ByteSwap16_ROR8) {
    uint32_t x=nextReg(), r_=nextReg();
    IdiomInstr ror;
    ror.op=IdiomOp::Ror; ror.dst=IdiomOperand::reg32(r_); ror.dst.width=16;
    ror.src0=IdiomOperand::reg32(x); ror.src0.width=16;
    ror.src1=IdiomOperand::makeImm(8,16);
    InstrWindow w = { ror };
    auto e = makeDefaultEngine();
    auto r = e.process(w);
    ASSERT_EQ(r.size(), 1u);
    EXPECT_EQ(r[0].kind, ReplacementKind::ByteSwap);
    EXPECT_EQ(r[0].operandWidth, 16u);
}

TEST(BitTest, ByteSwap32_Software) {
    uint32_t x=nextReg(), t0=nextReg(), t1=nextReg(), t2=nextReg(), t3=nextReg(), r_=nextReg();
    IdiomInstr ror;
    ror.op=IdiomOp::Ror; ror.dst=IdiomOperand::reg32(t0);
    ror.src0=IdiomOperand::reg32(x); ror.src1=IdiomOperand::makeImm(16);
    InstrWindow w = {
        ror,
        make(IdiomOp::And, t1, t0, (int64_t)0x00FF00FF),
        make(IdiomOp::Shr, t2, t0, 8),
        make(IdiomOp::And, t3, t2, (int64_t)0x00FF00FF),
        makeRR(IdiomOp::Or, r_, t1, t3),
    };
    auto e = makeDefaultEngine();
    auto r = e.process(w);
    ASSERT_EQ(r.size(), 1u);
    EXPECT_EQ(r[0].kind, ReplacementKind::ByteSwap);
    EXPECT_EQ(r[0].operandWidth, 32u);
}

// ─── SIMD memset / memcpy tests ───────────────────────────────────────────────

static IdiomInstr makeVecSet(uint32_t dst, int64_t val, uint32_t vecW) {
    IdiomInstr ins;
    ins.op=IdiomOp::VecSet; ins.dst=IdiomOperand::vreg(dst,vecW*8);
    ins.src0=IdiomOperand::makeImm(val,32); ins.vecWidth=vecW;
    return ins;
}
static IdiomInstr makeVecStore(uint32_t vecR, uint32_t base, int64_t off, uint32_t vecW) {
    IdiomInstr ins;
    ins.op=IdiomOp::VecStore; ins.vecWidth=vecW;
    ins.src0=IdiomOperand::reg32(vecR);  // data
    ins.dst=IdiomOperand::reg32(base);   // base (repurposed as dst mem base)
    ins.src1=IdiomOperand::makeImm(off);     // offset
    return ins;
}
static IdiomInstr makeVecLoad(uint32_t dst, uint32_t base, int64_t off, uint32_t vecW) {
    IdiomInstr ins;
    ins.op=IdiomOp::VecLoad; ins.vecWidth=vecW;
    ins.dst=IdiomOperand::vreg(dst,vecW*8);
    ins.src0=IdiomOperand::reg32(base);
    ins.src1=IdiomOperand::makeImm(off);
    return ins;
}

TEST(SimdMemTest, Memset_2xSSE) {
    uint32_t vr=nextReg(), dst=nextReg();
    InstrWindow w = {
        makeVecSet(vr, 0, 16),
        makeVecStore(vr, dst, 0, 16),
        makeVecStore(vr, dst, 16, 16),
    };
    auto e = makeDefaultEngine();
    auto r = e.process(w);
    ASSERT_EQ(r.size(), 1u);
    EXPECT_EQ(r[0].kind, ReplacementKind::Memset);
    EXPECT_EQ(r[0].dstReg, dst);
    EXPECT_EQ(r[0].countImm, 32);
    EXPECT_EQ(r[0].fillValue, 0);
}

TEST(SimdMemTest, Memset_4xAVX) {
    uint32_t vr=nextReg(), dst=nextReg();
    InstrWindow w = {
        makeVecSet(vr, 0xFF, 32),
        makeVecStore(vr, dst, 0,   32),
        makeVecStore(vr, dst, 32,  32),
        makeVecStore(vr, dst, 64,  32),
        makeVecStore(vr, dst, 96,  32),
    };
    auto e = makeDefaultEngine();
    auto r = e.process(w);
    ASSERT_EQ(r.size(), 1u);
    EXPECT_EQ(r[0].kind, ReplacementKind::Memset);
    EXPECT_EQ(r[0].countImm, 128);
}

TEST(SimdMemTest, Memcpy_2xSSE) {
    uint32_t src=nextReg(), dst=nextReg(), v0=nextReg(), v1=nextReg();
    InstrWindow w = {
        makeVecLoad(v0,  src, 0,  16),
        makeVecStore(v0, dst, 0,  16),
        makeVecLoad(v1,  src, 16, 16),
        makeVecStore(v1, dst, 16, 16),
    };
    auto e = makeDefaultEngine();
    auto r = e.process(w);
    ASSERT_EQ(r.size(), 1u);
    EXPECT_EQ(r[0].kind, ReplacementKind::Memcpy);
    EXPECT_EQ(r[0].srcReg, src);
    EXPECT_EQ(r[0].dstReg, dst);
    EXPECT_EQ(r[0].countImm, 32);
}

// ─── Engine: multiple idioms in one window ────────────────────────────────────

TEST(EngineTest, TwoIdiomsInSequence) {
    // [0..1] unsigned div by 7 (simple), [2..4] abs (form B)
    uint32_t x=nextReg(), q=0;
    auto magic = magicUnsigned32(7);
    auto divW  = buildUnsignedSimple(x, q, magic.M, magic.k);

    uint32_t y=nextReg(), t=nextReg(), t2=nextReg(), z=nextReg();
    InstrWindow absW = {
        make(IdiomOp::Sar, t,  y, 31),
        makeRR(IdiomOp::Xor, t2, y, t),
        makeRR(IdiomOp::Sub, z,  t2, t),
    };

    InstrWindow combined;
    for (auto& i : divW)  combined.push_back(i);
    for (auto& i : absW)  combined.push_back(i);

    auto e = makeDefaultEngine(CompilerProfile::GCC);
    auto r = e.process(combined);

    ASSERT_EQ(r.size(), 2u);
    EXPECT_EQ(r[0].kind, ReplacementKind::DivUnsigned);
    EXPECT_EQ(r[0].divisor, 7);
    EXPECT_EQ(r[1].kind, ReplacementKind::AbsValue);
}

TEST(EngineTest, NoMatchReturnsEmpty) {
    uint32_t x=nextReg(), y=nextReg();
    InstrWindow w = { makeRR(IdiomOp::Add, y, x, x) };
    auto e = makeDefaultEngine();
    EXPECT_TRUE(e.process(w).empty());
}

// ─── ReplacementNode::debugStr ────────────────────────────────────────────────

TEST(ReplacementNodeTest, DebugStrDiv) {
    ReplacementNode r;
    r.kind=ReplacementKind::DivUnsigned; r.inputReg=1; r.outputReg=2; r.divisor=7; r.operandWidth=32;
    r.firstVma=0x1000; r.lastVma=0x1008; r.instrCount=2;
    std::string s = r.debugStr();
    EXPECT_NE(s.find("7"), std::string::npos);
    EXPECT_NE(s.find("r2"), std::string::npos);
}

TEST(ReplacementNodeTest, DebugStrMemset) {
    ReplacementNode r;
    r.kind=ReplacementKind::Memset; r.dstReg=3; r.fillValue=0; r.countImm=64;
    std::string s = r.debugStr();
    EXPECT_NE(s.find("memset"), std::string::npos);
}
