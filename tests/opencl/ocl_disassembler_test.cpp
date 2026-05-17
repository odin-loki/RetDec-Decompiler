/**
 * @file tests/opencl/ocl_disassembler_test.cpp
 * @brief Tests for OCLDisassembler (parallel x86-64 CFG disassembly).
 *
 * The tests exercise the CPU-fallback path always.
 * Tests tagged [opencl] are additionally run on the GPU if available.
 */

#include <memory>
#include "retdec/opencl/ocl_disassembler.h"
#include "retdec/opencl/ocl_context.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <vector>

using namespace retdec::opencl;

// ─── Helper to build a small x86-64 code snippet ─────────────────────────────

static std::vector<std::uint8_t> makeRetCode()
{
    // xor eax, eax   (2 bytes: 31 C0)
    // ret            (1 byte:  C3)
    return { 0x31, 0xC0, 0xC3 };
}

static std::vector<std::uint8_t> makeJmpForwardCode()
{
    // jmp +2  (EB 02)
    // nop     (90)
    // nop     (90)
    // ret     (C3)
    return { 0xEB, 0x02, 0x90, 0x90, 0xC3 };
}

static std::vector<std::uint8_t> makeJccCode()
{
    // test eax, eax  (85 C0)
    // jz +3          (74 03)
    // xor eax, eax   (31 C0)
    // nop            (90)
    // ret            (C3)
    return { 0x85, 0xC0, 0x74, 0x03, 0x31, 0xC0, 0x90, 0xC3 };
}

static std::vector<std::uint8_t> makeTwoEntryCode()
{
    // Two independent blocks at offset 0 and offset 4:
    // [0] nop (90), nop (90), nop (90), ret (C3)
    // [4] push rbp (55), pop rbp (5D), ret (C3)
    return { 0x90, 0x90, 0x90, 0xC3, 0x55, 0x5D, 0xC3 };
}

// ─── Fixture ─────────────────────────────────────────────────────────────────

class OCLDisasmCPUTest : public ::testing::Test {
protected:
    // Always uses CPU fallback (null ctx).
    OCLDisassembler dis{nullptr};
};

class OCLDisasmGPUTest : public ::testing::Test {
protected:
    void SetUp() override
    {
        _gpuOk = _ctx.initialize();
        _dis    = std::make_unique<OCLDisassembler>(_gpuOk ? &_ctx : nullptr);
    }

    OCLContext                    _ctx;
    bool                          _gpuOk = false;
    std::unique_ptr<OCLDisassembler> _dis;
};

// ─── CPU tests ───────────────────────────────────────────────────────────────

TEST_F(OCLDisasmCPUTest, EmptyInputReturnsEmpty)
{
    auto result = dis.disassemble(nullptr, 0, 0, {});
    EXPECT_TRUE(result.empty());
}

TEST_F(OCLDisasmCPUTest, NoEntriesReturnsEmpty)
{
    auto code = makeRetCode();
    auto result = dis.disassemble(code.data(), code.size(), 0x1000, {});
    EXPECT_TRUE(result.empty());
}

TEST_F(OCLDisasmCPUTest, SimpleRetBlock)
{
    auto code = makeRetCode();
    const std::uint64_t base = 0x400000;
    auto result = dis.disassemble(code.data(), code.size(), base, {base});

    ASSERT_EQ(result.size(), 1u);
    EXPECT_EQ(result[0].startAddr, base);
    EXPECT_EQ(result[0].endAddr,   base + code.size()); // xor(2) + ret(1) = 3
    EXPECT_TRUE(result[0].endsWithRet());
    EXPECT_EQ(result[0].insnCount, 2u);
}

TEST_F(OCLDisasmCPUTest, UnconditionalJmpTerminatesBlock)
{
    auto code = makeJmpForwardCode();
    const std::uint64_t base = 0x0;
    auto result = dis.disassemble(code.data(), code.size(), base, {base});

    ASSERT_GE(result.size(), 1u);
    // First block ends with JMP at offset 0, length 2.
    auto& bb0 = result[0];
    EXPECT_TRUE(bb0.endsWithJmp());
    EXPECT_EQ(bb0.startAddr, 0x0u);
    EXPECT_EQ(bb0.insnCount, 1u);                        // only the jmp insn
    EXPECT_EQ(bb0.successor0, static_cast<std::uint64_t>(4u)); // jmp target = 0 + 2 + 2 = 4
}

TEST_F(OCLDisasmCPUTest, ConditionalJccProducesTwoSuccessors)
{
    auto code = makeJccCode();
    const std::uint64_t base = 0x2000;
    auto result = dis.disassemble(code.data(), code.size(), base, {base});

    ASSERT_GE(result.size(), 1u);
    auto& bb0 = result[0];
    EXPECT_TRUE(bb0.endsWithJcc());
    // Fall-through = base + 4 (past jz), taken = base + 4 + 3 = base + 7
    EXPECT_EQ(bb0.successor0, base + 4u);
    EXPECT_EQ(bb0.successor1, base + 4u + 3u);
}

TEST_F(OCLDisasmCPUTest, TwoIndependentEntries)
{
    auto code = makeTwoEntryCode();
    const std::uint64_t base = 0x0;
    auto result = dis.disassemble(
        code.data(), code.size(), base, {base, base + 4u});

    ASSERT_EQ(result.size(), 2u);
    EXPECT_EQ(result[0].startAddr, base);
    EXPECT_EQ(result[1].startAddr, base + 4u);
    EXPECT_TRUE(result[0].endsWithRet());
    EXPECT_TRUE(result[1].endsWithRet());
}

TEST_F(OCLDisasmCPUTest, OutputSortedByStartAddr)
{
    auto code = makeTwoEntryCode();
    const std::uint64_t base = 0x0;
    // Supply entries in reverse order.
    auto result = dis.disassemble(
        code.data(), code.size(), base, {base + 4u, base});

    ASSERT_EQ(result.size(), 2u);
    EXPECT_LT(result[0].startAddr, result[1].startAddr);
}

TEST_F(OCLDisasmCPUTest, DuplicateEntriesDeduped)
{
    auto code = makeRetCode();
    const std::uint64_t base = 0x100;
    auto result = dis.disassemble(
        code.data(), code.size(), base, {base, base, base});

    EXPECT_EQ(result.size(), 1u);
}

TEST_F(OCLDisasmCPUTest, InvalidEntryOutsideBufferFlagged)
{
    auto code = makeRetCode();
    const std::uint64_t base = 0x0;
    // Entry far outside the buffer.
    auto result = dis.disassemble(
        code.data(), code.size(), base, {0xDEAD0000ULL});

    // Should return either empty or an invalid-flagged block.
    for (const auto& bb : result) {
        EXPECT_TRUE(bb.isInvalid() || bb.insnCount == 0);
    }
}

// ─── CPU path: 50-function synthetic benchmark check ─────────────────────────

TEST_F(OCLDisasmCPUTest, FiftyFunctionSynthetic)
{
    // Build a buffer of 50 tiny functions: each is "xor eax,eax; ret".
    const std::vector<uint8_t> fnBytes = { 0x31, 0xC0, 0xC3 };
    const std::size_t fnSize = fnBytes.size();
    const std::size_t numFns = 50;

    std::vector<std::uint8_t> code;
    code.reserve(fnSize * numFns);
    for (std::size_t i = 0; i < numFns; ++i) {
        code.insert(code.end(), fnBytes.begin(), fnBytes.end());
    }

    std::vector<std::uint64_t> entries;
    for (std::size_t i = 0; i < numFns; ++i) {
        entries.push_back(static_cast<std::uint64_t>(i * fnSize));
    }

    auto result = dis.disassemble(code.data(), code.size(), 0, entries);

    ASSERT_EQ(result.size(), numFns);
    for (const auto& bb : result) {
        EXPECT_TRUE(bb.endsWithRet());
        EXPECT_EQ(bb.insnCount, 2u);
        EXPECT_EQ(bb.sizeBytes(), fnSize);
    }
}

// ─── GPU tests (skipped if no device) ────────────────────────────────────────

TEST_F(OCLDisasmGPUTest, GPUSimpleRetBlock)
{
    if (!_gpuOk) { GTEST_SKIP() << "No OpenCL device"; }

    auto code = makeRetCode();
    const std::uint64_t base = 0x400000;
    auto result = _dis->disassemble(code.data(), code.size(), base, {base});

    ASSERT_GE(result.size(), 1u);
    EXPECT_EQ(result[0].startAddr, base);
    EXPECT_TRUE(result[0].endsWithRet());
}

TEST_F(OCLDisasmGPUTest, GPUAndCPUAgree)
{
    if (!_gpuOk) { GTEST_SKIP() << "No OpenCL device"; }

    auto code = makeTwoEntryCode();
    const std::uint64_t base = 0x1000;

    OCLDisassembler cpuDis(nullptr);
    auto cpuResult = cpuDis.disassemble(code.data(), code.size(), base, {base, base + 4u});
    auto gpuResult = _dis->disassemble(code.data(), code.size(), base, {base, base + 4u});

    // Both paths must agree on the number of basic blocks and their addresses.
    ASSERT_EQ(cpuResult.size(), gpuResult.size());
    for (std::size_t i = 0; i < cpuResult.size(); ++i) {
        EXPECT_EQ(cpuResult[i].startAddr, gpuResult[i].startAddr);
        EXPECT_EQ(cpuResult[i].endAddr,   gpuResult[i].endAddr);
        EXPECT_EQ(cpuResult[i].flags,     gpuResult[i].flags);
    }
}
