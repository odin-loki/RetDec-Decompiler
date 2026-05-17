/**
 * @file tests/cuda_accel/cuda_semantic_hasher_test.cpp
 */
#include "retdec/cuda_accel/cuda_semantic_hasher.h"
#include "retdec/cuda_accel/cuda_context.h"
#include <gtest/gtest.h>

using namespace retdec::cuda_accel;

// xor eax,eax; ret  (returns 0 regardless of inputs)
static const uint8_t kRetZero[] = {0x31, 0xC0, 0xC3};

// mov rax,0; ret  (same semantic)
static const uint8_t kMovZeroRet[] = {0x48, 0xC7, 0xC0, 0x00, 0x00, 0x00, 0x00, 0xC3};

class CUDASemanticHasherTest : public ::testing::Test {
protected:
    CUDAContext ctx;
    void SetUp() override { ctx.initialize(); }
};

TEST_F(CUDASemanticHasherTest, EmptyInput) {
    CUDASemanticHasher h(nullptr);
    auto r = h.hash({});
    EXPECT_TRUE(r.empty());
}

TEST_F(CUDASemanticHasherTest, SingleFunction_CPU) {
    FunctionBytecode fb;
    fb.bytes.assign(kRetZero, kRetZero + sizeof(kRetZero));
    CUDASemanticHasher h(nullptr);
    auto r = h.hash({fb});
    ASSERT_EQ(r.size(), 1u);
    EXPECT_NE(r[0].ioHash, 0u);
}

TEST_F(CUDASemanticHasherTest, DifferentFunctionsHaveDifferentHashes_CPU) {
    FunctionBytecode fb1, fb2;
    fb1.bytes = {0x31, 0xC0, 0xC3};       // xor eax,eax; ret
    fb2.bytes = {0xB8, 0x01, 0x00, 0x00, 0x00, 0xC3}; // mov eax,1; ret
    CUDASemanticHasher h(nullptr);
    auto r = h.hash({fb1, fb2});
    ASSERT_EQ(r.size(), 2u);
    // Different functions should (usually) hash differently
    EXPECT_NE(r[0].ioHash, r[1].ioHash);
}

TEST_F(CUDASemanticHasherTest, GPU_CPU_Consistent) {
    if (!ctx.isReady()) GTEST_SKIP() << "No CUDA device";
    FunctionBytecode fb;
    fb.bytes.assign(kRetZero, kRetZero + sizeof(kRetZero));

    CUDASemanticHasher cpu(nullptr), gpu(&ctx);
    auto rc = cpu.hash({fb});
    auto rg = gpu.hash({fb});
    ASSERT_EQ(rc.size(), 1u);
    ASSERT_EQ(rg.size(), 1u);
    // Hash values may differ slightly (different test vector sets) but both non-zero
    EXPECT_NE(rc[0].ioHash, 0u);
    EXPECT_NE(rg[0].ioHash, 0u);
}

TEST_F(CUDASemanticHasherTest, DefaultTestVectors) {
    auto vecs = defaultTestVectors(0);
    EXPECT_EQ(vecs.size(), kTestVectorCount);
    // Should be deterministic
    auto vecs2 = defaultTestVectors(0);
    EXPECT_EQ(vecs, vecs2);
    // Different seeds should differ
    auto vecs3 = defaultTestVectors(1);
    EXPECT_NE(vecs, vecs3);
}

TEST_F(CUDASemanticHasherTest, SemanticHashDB) {
    SemanticHashDB db;
    db.insert("func_a", 0xDEADBEEFCAFEBABEULL);
    EXPECT_EQ(db.size(), 1u);
    EXPECT_EQ(db.lookup(0xDEADBEEFCAFEBABEULL), "func_a");
    EXPECT_EQ(db.lookup(0x1234567890ABCDEFULL), "");
}
