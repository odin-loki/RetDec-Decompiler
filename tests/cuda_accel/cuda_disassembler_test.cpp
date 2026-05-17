/**
 * @file tests/cuda_accel/cuda_disassembler_test.cpp
 * @brief Unit tests for CUDADisassembler (both GPU and CPU paths).
 */
#include "retdec/cuda_accel/cuda_disassembler.h"
#include "retdec/cuda_accel/cuda_context.h"
#include <gtest/gtest.h>

using namespace retdec::cuda_accel;

// Simple x86-64 function: xor eax,eax; ret
static const uint8_t kXorRet[] = {0x31, 0xC0, 0xC3};

// Short unconditional JMP followed by a NOP and a RET
static const uint8_t kJmpNopRet[] = {
    0xEB, 0x01,  // JMP +1 (over the NOP)
    0x90,        // NOP (jumped over)
    0xC3,        // RET
};

static const uint8_t kCallRet[] = {
    0xE8, 0x00, 0x00, 0x00, 0x00,  // CALL +0 (call to next insn; simplified)
    0xC3,
};

class CUDADisassemblerTest : public ::testing::Test {
protected:
    CUDAContext ctx;
    void SetUp() override { ctx.initialize(); }
};

TEST_F(CUDADisassemblerTest, DisassembleXorRet_CPU) {
    CUDADisassembler dis(nullptr); // force CPU path
    auto bbs = dis.disassemble(kXorRet, sizeof(kXorRet), 0x1000,
                                {0x1000});
    ASSERT_EQ(bbs.size(), 1u);
    EXPECT_EQ(bbs[0].startAddr, 0x1000u);
    EXPECT_TRUE(bbs[0].endsWithRet());
    EXPECT_EQ(bbs[0].insnCount, 2u);
}

TEST_F(CUDADisassemblerTest, DisassembleJmpNopRet_CPU) {
    CUDADisassembler dis(nullptr);
    auto bbs = dis.disassemble(kJmpNopRet, sizeof(kJmpNopRet), 0x2000,
                                {0x2000});
    ASSERT_GE(bbs.size(), 1u);
    // First BB should end with JMP
    EXPECT_TRUE(bbs[0].endsWithJmp());
}

TEST_F(CUDADisassemblerTest, DisassembleXorRet_GPU) {
    if (!ctx.isReady()) GTEST_SKIP() << "No CUDA device";
    CUDADisassembler dis(&ctx);
    EXPECT_TRUE(dis.usesGPU());

    auto bbs = dis.disassemble(kXorRet, sizeof(kXorRet), 0x1000, {0x1000});
    ASSERT_EQ(bbs.size(), 1u);
    EXPECT_TRUE(bbs[0].endsWithRet());
}

TEST_F(CUDADisassemblerTest, MultipleSeeds_CPU) {
    CUDADisassembler dis(nullptr);
    std::vector<uint64_t> seeds = {0x1000, 0x1002};
    auto bbs = dis.disassemble(kXorRet, sizeof(kXorRet), 0x1000, seeds);
    EXPECT_EQ(bbs.size(), 2u);
}

TEST_F(CUDADisassemblerTest, EmptyInput) {
    CUDADisassembler dis(nullptr);
    auto bbs = dis.disassemble(nullptr, 0, 0, {});
    EXPECT_TRUE(bbs.empty());
}

TEST_F(CUDADisassemblerTest, InvalidSeed) {
    CUDADisassembler dis(nullptr);
    auto bbs = dis.disassemble(kXorRet, sizeof(kXorRet), 0x1000, {0xDEADBEEF});
    ASSERT_EQ(bbs.size(), 1u);
    EXPECT_TRUE(bbs[0].isInvalid() || bbs[0].startAddr == kBBAddrNone);
}
