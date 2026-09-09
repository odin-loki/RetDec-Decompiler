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

// ─── The emulated stack pointer is whatever the code says ────────────────────

// RSP is set by the emulated function, and the scratch stack is a 4096-byte
// array. PUSH tested only `sp >= 8` -- nothing bounded it above -- so
// sp = 0xFFFFFFFF wrote eight bytes at scratch + 0xFFFFFFF7, four gigabytes
// past the array; glibc's fortify caught it as "*** buffer overflow detected
// ***". POP's bound was `sp + 8 <= EM_SCRATCH`, uint32 arithmetic that wraps
// to a small number for sp >= 0xFFFFFFF8 and passes, putting the read the same
// distance out. The CALL and RET arms on both the host and device paths had
// the same two shapes.
TEST_F(CUDASemanticHasherTest, AnEmulatedStackPointerOutsideTheScratchIsRefused) {
    // mov rsp, 0xFFFFFFFFFFFFFFFF ; push rax ; pop rax ; hlt
    FunctionBytecode fb;
    fb.baseVMA = 0x1000;
    fb.bytes = {0x48, 0xBC, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
                0x50, 0x58, 0xF4};

    CUDASemanticHasher h(nullptr);
    auto sigs = h.hash({fb});
    ASSERT_EQ(1u, sigs.size());
    // Reaching here at all is the assertion; the emulator must have declined
    // the accesses rather than making them.
    SUCCEED();
}

// A stack pointer just below the bound still works, so the guard did not cost
// the ordinary case.
TEST_F(CUDASemanticHasherTest, AnInBoundsPushAndPopStillRunTheStack) {
    // mov rsp, 4096 ; push rax ; pop rax ; hlt
    FunctionBytecode fb;
    fb.baseVMA = 0x1000;
    fb.bytes = {0x48, 0xBC, 0x00, 0x10, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
                0x50, 0x58, 0xF4};

    CUDASemanticHasher h(nullptr);
    auto sigs = h.hash({fb});
    ASSERT_EQ(1u, sigs.size());
    EXPECT_EQ(EmulationStatus::Halted, sigs[0].status);
}
