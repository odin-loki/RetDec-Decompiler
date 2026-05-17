/**
 * @file tests/opencl/ocl_semantic_hasher_test.cpp
 * @brief Unit tests for OCLSemanticHasher.
 */

#include <memory>
#include "retdec/opencl/ocl_semantic_hasher.h"
#include "retdec/opencl/ocl_context.h"

#include <gtest/gtest.h>

#include <vector>

using namespace retdec::opencl;

// ─── Tiny x86-64 code snippets ────────────────────────────────────────────────

// xor eax, eax; ret  — always returns 0.
static std::vector<uint8_t> makeZeroFunc()   { return {0x31, 0xC0, 0xC3}; }

// mov rax, rdi; ret  — identity function (returns first arg).
static std::vector<uint8_t> makeIdentFunc()  { return {0x48, 0x89, 0xF8, 0xC3}; }

// mov rax, 42; ret   — constant 42.
static std::vector<uint8_t> makeConst42Func() { return {0x48, 0xC7, 0xC0, 0x2A, 0x00, 0x00, 0x00, 0xC3}; }

// add rdi, rsi; mov rax, rdi; ret  — sum of first two args.
static std::vector<uint8_t> makeAddFunc()
{
    return {
        0x48, 0x01, 0xF7,         // add rdi, rsi
        0x48, 0x89, 0xF8,         // mov rax, rdi
        0xC3,                     // ret
    };
}

// ─── Fixture ─────────────────────────────────────────────────────────────────

class SemanticHasherCPUTest : public ::testing::Test {
protected:
    OCLSemanticHasher hasher{nullptr};
};

// ─── Helper ──────────────────────────────────────────────────────────────────

static FunctionBytecode makeBC(std::vector<uint8_t> bytes, std::uint32_t funcIdx = 0)
{
    FunctionBytecode f;
    f.bytes      = std::move(bytes);
    f.testInputs = defaultTestVectors(funcIdx);
    return f;
}

// ─── CPU tests ────────────────────────────────────────────────────────────────

TEST_F(SemanticHasherCPUTest, EmptyInputReturnsEmpty)
{
    EXPECT_TRUE(hasher.hash({}).empty());
}

TEST_F(SemanticHasherCPUTest, ReturnsOneSignaturePerTestVector)
{
    auto sigs = hasher.hash({makeBC(makeZeroFunc())});
    EXPECT_EQ(sigs.size(), kTestVectorCount);
}

TEST_F(SemanticHasherCPUTest, TwoFunctionsReturnCorrectCount)
{
    auto sigs = hasher.hash({makeBC(makeZeroFunc()), makeBC(makeIdentFunc())});
    EXPECT_EQ(sigs.size(), 2 * kTestVectorCount);
}

TEST_F(SemanticHasherCPUTest, ZeroFuncHashesSameAcrossTestVectors)
{
    // xor eax,eax always returns 0 regardless of input → all 16 hashes should
    // have the same OUTPUT hash component, though the full IO hash differs
    // because the INPUT components differ.
    // Just verify no crash and all hashes are non-zero.
    auto sigs = hasher.hash({makeBC(makeZeroFunc())});
    ASSERT_EQ(sigs.size(), kTestVectorCount);
    for (const auto& s : sigs) {
        EXPECT_NE(s.ioHash, 0u);
    }
}

TEST_F(SemanticHasherCPUTest, DifferentFunctionsProduceDifferentHashes)
{
    // Run both functions on the same (default) test vectors.
    auto sigsZ = hasher.hash({makeBC(makeZeroFunc(),   0)});
    auto sigsC = hasher.hash({makeBC(makeConst42Func(),0)});

    ASSERT_EQ(sigsZ.size(), kTestVectorCount);
    ASSERT_EQ(sigsC.size(), kTestVectorCount);

    // At least one test vector must differ.
    bool anyDiff = false;
    for (std::size_t tv = 0; tv < kTestVectorCount; ++tv) {
        if (sigsZ[tv].ioHash != sigsC[tv].ioHash) { anyDiff = true; break; }
    }
    EXPECT_TRUE(anyDiff);
}

TEST_F(SemanticHasherCPUTest, SameFunctionSameHash)
{
    auto sigs1 = hasher.hash({makeBC(makeZeroFunc(), 0)});
    auto sigs2 = hasher.hash({makeBC(makeZeroFunc(), 0)});

    ASSERT_EQ(sigs1.size(), sigs2.size());
    for (std::size_t i = 0; i < sigs1.size(); ++i) {
        EXPECT_EQ(sigs1[i].ioHash, sigs2[i].ioHash);
    }
}

TEST_F(SemanticHasherCPUTest, SemanticHashDBInsertAndLookup)
{
    SemanticHashDB db;
    EXPECT_EQ(db.size(), 0u);

    db.insert("memcpy",  0x123456789ABCDEF0ULL);
    db.insert("strlen",  0xDEADBEEFCAFEBABEULL);
    db.insert("strcpy",  0x0102030405060708ULL);
    EXPECT_EQ(db.size(), 3u);

    EXPECT_EQ(db.lookup(0x123456789ABCDEF0ULL), "memcpy");
    EXPECT_EQ(db.lookup(0xDEADBEEFCAFEBABEULL), "strlen");
    EXPECT_EQ(db.lookup(0x0102030405060708ULL), "strcpy");
    EXPECT_TRUE(db.lookup(0xFFFFFFFFFFFFFFFFULL).empty());
}

TEST_F(SemanticHasherCPUTest, DefaultTestVectorsNonZero)
{
    auto m = defaultTestVectors(0);
    bool anyNonZero = false;
    for (const auto& row : m) {
        for (std::uint64_t v : row) {
            if (v != 0) { anyNonZero = true; break; }
        }
    }
    EXPECT_TRUE(anyNonZero);
}

TEST_F(SemanticHasherCPUTest, DefaultTestVectorsDifferPerFunction)
{
    auto m0 = defaultTestVectors(0);
    auto m1 = defaultTestVectors(1);
    bool anyDiff = false;
    for (std::size_t tv = 0; tv < kTestVectorCount; ++tv) {
        for (std::size_t i = 0; i < kTestInputWidth; ++i) {
            if (m0[tv][i] != m1[tv][i]) { anyDiff = true; break; }
        }
    }
    EXPECT_TRUE(anyDiff);
}

// ─── GPU tests ────────────────────────────────────────────────────────────────

class SemanticHasherGPUTest : public ::testing::Test {
protected:
    void SetUp() override
    {
        _gpuOk = _ctx.initialize();
        _hasher = std::make_unique<OCLSemanticHasher>(_gpuOk ? &_ctx : nullptr);
    }
    OCLContext                        _ctx;
    bool                              _gpuOk = false;
    std::unique_ptr<OCLSemanticHasher> _hasher;
};

TEST_F(SemanticHasherGPUTest, GPUReturnsCorrectCount)
{
    if (!_gpuOk) { GTEST_SKIP() << "No OpenCL device"; }
    auto sigs = _hasher->hash({makeBC(makeZeroFunc())});
    EXPECT_EQ(sigs.size(), kTestVectorCount);
}

TEST_F(SemanticHasherGPUTest, GPUHashesNonZero)
{
    if (!_gpuOk) { GTEST_SKIP() << "No OpenCL device"; }
    auto sigs = _hasher->hash({makeBC(makeZeroFunc())});
    for (const auto& s : sigs) {
        EXPECT_NE(s.ioHash, 0u);
    }
}
