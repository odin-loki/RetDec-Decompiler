/**
 * @file tests/cuda_accel/cuda_disassembler_test.cpp
 * @brief Unit tests for CUDADisassembler (both GPU and CPU paths).
 */
#include "retdec/cuda_accel/cuda_disassembler.h"
#include "retdec/cuda_accel/cuda_context.h"
#include <gtest/gtest.h>

#include <cstdint>
#include <sstream>
#include <string>
#include <vector>

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

// ─── the same bytes, twice ──────────────────────────────────────────────────
//
// disassembleCPU dispatched its seeds with std::async against one shared
// visited map, and each walk stopped at the first byte any other seed had
// already decoded. Which seed got there first was up to the scheduler, so the
// basic-block set for one input was not a function of that input: 200 runs of
// the 4 KiB case below produced 14, 30 and 24 distinct results on three
// attempts before the fix.

namespace {

std::string fingerprint(const std::vector<BasicBlock>& bbs)
{
	std::ostringstream os;
	for (const auto& b: bbs)
		os << b.startAddr << ':' << b.endAddr << ':' << b.successor0 << ':' << b.successor1 << ':' << b.insnCount << ':'
		   << b.flags << ' ';
	return os.str();
}

} // namespace

TEST_F(CUDADisassemblerTest, TheSameInputGivesTheSameBlocksEveryTime)
{
	// Overlapping seeds are the point: every seed's walk runs into the bytes a
	// neighbour is also decoding.
	const std::size_t kSize = 4096;
	std::vector<std::uint8_t> code(kSize, 0x90); // NOP run
	code.back() = 0xC3;                          // RET

	std::vector<std::uint64_t> seeds;
	for (std::size_t i = 0; i < kSize; i += 16)
		seeds.push_back(0x400000 + i);

	CUDADisassembler dis(nullptr);
	const std::string first = fingerprint(dis.disassemble(code.data(), code.size(), 0x400000, seeds));

	for (int run = 0; run < 40; ++run)
	{
		const std::string again = fingerprint(dis.disassemble(code.data(), code.size(), 0x400000, seeds));
		ASSERT_EQ(first, again) << "run " << run << " disagreed with the first";
	}
}

// The same over bytes that reach the branch, call and invalid-length paths
// rather than one long straight run.
TEST_F(CUDADisassemblerTest, DenseSeedsOverArbitraryBytesAreAlsoStable)
{
	const std::size_t kSize = 8192;
	std::vector<std::uint8_t> code(kSize);
	std::uint32_t x = 0x1234567u;
	for (auto& b: code)
	{
		x = x * 1103515245u + 12345u;
		b = static_cast<std::uint8_t>(x >> 16);
	}

	std::vector<std::uint64_t> seeds;
	for (std::size_t i = 0; i < kSize; i += 7)
		seeds.push_back(0x400000 + i);

	CUDADisassembler dis(nullptr);
	const std::string first = fingerprint(dis.disassemble(code.data(), code.size(), 0x400000, seeds));
	ASSERT_FALSE(first.empty());

	for (int run = 0; run < 20; ++run)
	{
		const std::string again = fingerprint(dis.disassemble(code.data(), code.size(), 0x400000, seeds));
		ASSERT_EQ(first, again) << "run " << run << " disagreed with the first";
	}
}

// A seed that lands inside a block an earlier seed already decoded is reported
// as an edge into it -- no instructions of its own, and successor0 naming where
// it landed. That is what the shared visited map is for, and it survives the
// split into decode-then-reconcile.
TEST_F(CUDADisassemblerTest, ASeedInsideAnEarlierBlockPointsIntoIt)
{
	const uint8_t code[] = {0x90, 0x90, 0x90, 0xC3}; // NOP NOP NOP RET
	CUDADisassembler dis(nullptr);

	auto bbs = dis.disassemble(code, sizeof(code), 0x3000, {0x3000, 0x3002});
	ASSERT_EQ(bbs.size(), 2u);

	EXPECT_EQ(bbs[0].startAddr, 0x3000u);
	EXPECT_TRUE(bbs[0].endsWithRet());

	EXPECT_EQ(bbs[1].startAddr, 0x3002u);
	EXPECT_EQ(bbs[1].insnCount, 0u);
	EXPECT_EQ(bbs[1].successor0, 0x3002u);
	EXPECT_FALSE(bbs[1].endsWithRet());
}
