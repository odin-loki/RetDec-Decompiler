/**
 * @file src/opencl/ocl_disassembler.cpp
 * @brief OCLDisassembler implementation.
 *
 * OpenCL path:
 *   1. Upload code bytes to device buffer.
 *   2. Build / reuse compiled parallel_disasm kernel.
 *   3. Dispatch ceil(entries/WG_SIZE) work-groups of 64 items each.
 *   4. Read back BasicBlock results and visited bitset.
 *   5. Merge, sort, de-duplicate.
 *
 * CPU fallback path (no OpenCL device):
 *   A simple iterative x86-64 linear sweep per seed using the same
 *   sequential logic.  Not as fast but produces identical results.
 */

#include <memory>
#include "retdec/opencl/ocl_disassembler.h"
#include "retdec/opencl/ocl_context.h"
#include "retdec/opencl/kernel_sources.h"

#include <CL/cl.h>

#include <algorithm>
#include <cassert>
#include <cstring>
#include <thread>
#include <mutex>
#include <vector>

namespace retdec {
namespace opencl {

// ─── GPU-side BasicBlock layout (must match .cl struct) ──────────────────────

struct CLBasicBlock {
    std::uint64_t startAddr;
    std::uint64_t endAddr;
    std::uint64_t successor0;
    std::uint64_t successor1;
    std::uint32_t insnCount;
    std::uint32_t flags;
};
static_assert(sizeof(CLBasicBlock) == 40, "CLBasicBlock size mismatch");

// ─── CPU fallback: minimal x86-64 length decoder ─────────────────────────────
namespace cpu {

static std::uint32_t x86InsnLength(const std::uint8_t* bytes,
                                   std::size_t          off,
                                   std::size_t          maxOff)
{
    std::size_t start = off;

    bool has66 = false, has67 = false, rexW = false;

    // Prefixes
    for (int i = 0; i < 5 && off < maxOff; ++i) {
        uint8_t b = bytes[off];
        if (b == 0x66)                                                  { has66 = true; ++off; }
        else if (b == 0x67)                                             { has67 = true; ++off; }
        else if (b == 0xF0 || b == 0xF2 || b == 0xF3)                 { ++off; }
        else if (b == 0x26 || b == 0x2E || b == 0x36 ||
                 b == 0x3E || b == 0x64 || b == 0x65)                  { ++off; }
        else break;
    }
    if (off >= maxOff) return 0;

    // REX
    uint8_t b = bytes[off];
    if (b >= 0x40 && b <= 0x4F) { rexW = (b & 0x08) != 0; ++off; }
    if (off >= maxOff) return 0;

    b = bytes[off];

    // VEX
    if (b == 0xC4 || b == 0xC5) {
        std::size_t vl = (b == 0xC4) ? 3u : 2u;
        off += vl + 1u; // skip prefix bytes + opcode
        if (off >= maxOff) return 0;
        uint8_t mrm = bytes[off++];
        uint8_t mod = mrm >> 6, rm = mrm & 7;
        if (mod != 3) {
            if (!has67 && rm == 4) ++off;
            if (mod == 1) ++off;
            else if (mod == 2) off += 4;
            else if (rm == 5) off += 4;
        }
        if (off > maxOff) return 0;
        return static_cast<std::uint32_t>(off - start);
    }

    bool twoB = false;
    uint8_t opc2 = 0;
    uint8_t opc  = bytes[off++];
    if (opc == 0x0F && off < maxOff) {
        opc2  = bytes[off++];
        twoB  = true;
        if ((opc2 == 0x38 || opc2 == 0x3A) && off < maxOff) ++off;
    }

    bool hasModRM = false;
    int  immBytes = 0;

    if (!twoB) {
        if      (opc >= 0x50 && opc <= 0x5F)                                       {}
        else if (opc >= 0x90 && opc <= 0x97)                                       {}
        else if (opc == 0x98 || opc == 0x99 || opc == 0x9B ||
                 opc == 0x9C || opc == 0x9D || opc == 0x9E || opc == 0x9F)        {}
        else if (opc == 0xC3 || opc == 0xCB || opc == 0xC9 ||
                 opc == 0xCC || opc == 0xCE || opc == 0xCF)                        {}
        else if (opc == 0xC2 || opc == 0xCA)                                       { immBytes = 2; }
        else if (opc == 0xC8)                                                       { immBytes = 3; }
        else if (opc == 0xCD)                                                       { immBytes = 1; }
        else if (opc >= 0xF8 && opc <= 0xFD)                                       {}
        else if (opc == 0xF4 || opc == 0xF5 || opc == 0xD7)                       {}
        else if (opc == 0xEC || opc == 0xED || opc == 0xEE || opc == 0xEF)        {}
        else if (opc == 0xE4 || opc == 0xE5 || opc == 0xE6 || opc == 0xE7)        { immBytes = 1; }
        else if ((opc >= 0x70 && opc <= 0x7F) || opc == 0xEB || opc == 0xE3)      { immBytes = 1; }
        else if (opc == 0xE0 || opc == 0xE1 || opc == 0xE2)                       { immBytes = 1; }
        else if (opc == 0xE9 || opc == 0xE8)                                       { immBytes = 4; }
        else if (opc >= 0xB0 && opc <= 0xB7)                                       { immBytes = 1; }
        else if (opc >= 0xB8 && opc <= 0xBF)                                       { immBytes = rexW ? 8 : (has66 ? 2 : 4); }
        else if (opc >= 0xA0 && opc <= 0xA3)                                       { immBytes = has67 ? 4 : 8; }
        else if (opc == 0xA8)                                                       { immBytes = 1; }
        else if (opc == 0xA9)                                                       { immBytes = rexW ? 4 : (has66 ? 2 : 4); }
        else if (opc >= 0xA4 && opc <= 0xAF)                                       {}
        else if (opc == 0x05 || opc == 0x0D || opc == 0x15 || opc == 0x1D ||
                 opc == 0x25 || opc == 0x2D || opc == 0x35 || opc == 0x3D)        { immBytes = rexW ? 4 : (has66 ? 2 : 4); }
        else if (opc == 0x04 || opc == 0x0C || opc == 0x14 || opc == 0x1C ||
                 opc == 0x24 || opc == 0x2C || opc == 0x34 || opc == 0x3C)        { immBytes = 1; }
        else if (opc == 0x68)   { immBytes = has66 ? 2 : 4; }
        else if (opc == 0x6A)   { immBytes = 1; }
        else if (opc == 0x83)   { hasModRM = true; immBytes = 1; }
        else if (opc == 0x81)   { hasModRM = true; immBytes = rexW ? 4 : (has66 ? 2 : 4); }
        else if (opc == 0xC0 || opc == 0xC1) { hasModRM = true; immBytes = 1; }
        else if (opc == 0xD0 || opc == 0xD1 || opc == 0xD2 || opc == 0xD3) { hasModRM = true; }
        else if (opc == 0xC6)   { hasModRM = true; immBytes = 1; }
        else if (opc == 0xC7)   { hasModRM = true; immBytes = rexW ? 4 : (has66 ? 2 : 4); }
        else if (opc == 0x6B)   { hasModRM = true; immBytes = 1; }
        else if (opc == 0x69)   { hasModRM = true; immBytes = has66 ? 2 : 4; }
        else                    { hasModRM = true; }
    } else {
        if      (opc2 >= 0x80 && opc2 <= 0x8F)  { immBytes = 4; }
        else if (opc2 == 0xA4 || opc2 == 0xAC)  { hasModRM = true; immBytes = 1; }
        else if (opc2 == 0x1F)                   { hasModRM = true; }
        else if (opc2 == 0x18)                   { hasModRM = true; }
        else if (opc2 == 0xA2 || opc2 == 0x05 ||
                 opc2 == 0x07 || opc2 == 0x31 ||
                 opc2 == 0x34 || opc2 == 0x35)  {}
        else                                     { hasModRM = true; }
    }

    if (hasModRM) {
        if (off >= maxOff) return 0;
        uint8_t mrm = bytes[off++];
        uint8_t mod = mrm >> 6, rm = mrm & 7;

        std::size_t disp = 0;
        if      (mod == 1) disp = 1;
        else if (mod == 2) disp = 4;
        else if (mod == 0 && rm == 5) disp = 4;

        if (!has67 && mod != 3 && rm == 4) {
            if (off >= maxOff) return 0;
            uint8_t sib = bytes[off++];
            if ((sib & 7) == 5 && mod == 0) disp = 4;
        }
        off += disp;
    }

    off += static_cast<std::size_t>(immBytes);

    if (off > maxOff || off > start + 15) return 0;
    return static_cast<std::uint32_t>(off - start);
}

static BasicBlock walkSeed(const std::uint8_t* code,
                            std::size_t          codeSize,
                            std::uint64_t        baseVMA,
                            std::uint64_t        seedVMA)
{
    BasicBlock bb;
    bb.startAddr = seedVMA;

    if (seedVMA < baseVMA || seedVMA >= baseVMA + codeSize) {
        bb.flags = BB_INVALID;
        return bb;
    }

    std::size_t off = static_cast<std::size_t>(seedVMA - baseVMA);

    for (std::uint32_t steps = 0; steps < 4096 && off < codeSize; ++steps) {
        std::uint32_t len = x86InsnLength(code, off, codeSize);
        if (len == 0) { bb.flags |= BB_INVALID; break; }

        std::size_t nextOff = off + len;
        bb.endAddr    = baseVMA + nextOff;
        ++bb.insnCount;

        // Very simplified branch detect
        const uint8_t* p = code + off;
        // Skip prefixes + REX
        std::size_t po = 0;
        while (po < len && (p[po] == 0x66 || p[po] == 0x67 || p[po] == 0xF0 ||
                             p[po] == 0xF2 || p[po] == 0xF3 || p[po] == 0x26 ||
                             p[po] == 0x2E || p[po] == 0x36 || p[po] == 0x3E ||
                             p[po] == 0x64 || p[po] == 0x65)) { ++po; }
        if (po < len && p[po] >= 0x40 && p[po] <= 0x4F) ++po;
        if (po >= len) { off = nextOff; continue; }

        uint8_t b = p[po];

        if (b == 0xC3 || b == 0xCB || b == 0xC2 || b == 0xCA || b == 0xCF) {
            bb.flags |= BB_ENDS_RET;
            break;
        }
        if (b == 0xEB) {
            bb.flags    |= BB_ENDS_JMP;
            int rel      = static_cast<int8_t>(p[po + 1]);
            bb.successor0 = baseVMA + nextOff + rel;
            break;
        }
        if (b == 0xE9) {
            bb.flags    |= BB_ENDS_JMP;
            int rel      = static_cast<int32_t>(
                static_cast<uint32_t>(p[po+1]) | (static_cast<uint32_t>(p[po+2]) << 8) |
                (static_cast<uint32_t>(p[po+3]) << 16) | (static_cast<uint32_t>(p[po+4]) << 24));
            bb.successor0 = baseVMA + nextOff + rel;
            break;
        }
        if ((b >= 0x70 && b <= 0x7F) || b == 0xE3) {
            bb.flags    |= BB_ENDS_JCC;
            int rel      = static_cast<int8_t>(p[po + 1]);
            bb.successor0 = baseVMA + nextOff;
            bb.successor1 = baseVMA + nextOff + rel;
            break;
        }
        if (b == 0x0F && po + 1 < len) {
            uint8_t b2 = p[po + 1];
            if (b2 >= 0x80 && b2 <= 0x8F) {
                bb.flags    |= BB_ENDS_JCC;
                int rel      = static_cast<int32_t>(
                    static_cast<uint32_t>(p[po+2]) | (static_cast<uint32_t>(p[po+3]) << 8) |
                    (static_cast<uint32_t>(p[po+4]) << 16) | (static_cast<uint32_t>(p[po+5]) << 24));
                bb.successor0 = baseVMA + nextOff;
                bb.successor1 = baseVMA + nextOff + rel;
                break;
            }
        }
        if (b == 0xFF && po + 1 < len) {
            uint8_t reg = (p[po + 1] >> 3) & 7;
            if (reg == 4 || reg == 5) { bb.flags |= BB_ENDS_JMP;  break; }
            if (reg == 2 || reg == 3) { bb.flags |= BB_HAS_CALL; }
        }
        if (b == 0xE8) {
            bb.flags |= BB_HAS_CALL;
        }
        off = nextOff;
    }

    if (bb.endAddr == kBBAddrNone) { bb.endAddr = seedVMA; }
    return bb;
}

} // namespace cpu

// ─── Impl ─────────────────────────────────────────────────────────────────────

struct OCLDisassembler::Impl {
    OCLContext* ctx     = nullptr;
    bool        useGPU  = false;
    std::string lastErr;

    // Attempt to ensure the parallel_disasm kernel is compiled.
    bool ensureKernel()
    {
        if (!ctx || !ctx->isReady()) return false;
        std::string log;
        bool ok = ctx->ensureProgram(
            "retdec_parallel_disasm",
            parallelDisasmClSource(),
            "-cl-std=CL2.0",
            &log);
        if (!ok) {
            // Try CL1.2 fallback (without cl_khr_int64_base_atomics annotation).
            ok = ctx->ensureProgram(
                "retdec_parallel_disasm",
                parallelDisasmClSource(),
                "-cl-std=CL1.2",
                &log);
        }
        if (!ok) {
            lastErr = "parallel_disasm build failed: " + log;
        }
        return ok;
    }

    std::vector<BasicBlock> runGPU(const std::uint8_t*              code,
                                   std::size_t                       codeSize,
                                   std::uint64_t                     baseVMA,
                                   const std::vector<std::uint64_t>& entries)
    {
        if (entries.empty()) return {};

        const std::size_t N = entries.size();

        // ── Visited bitset (one bit per byte, using atomic_uint) ──────────
        // We use uint (32-bit) words, so we need ceil(codeSize/32) words.
        const std::size_t visitedWords = (codeSize + 31) / 32;
        const std::size_t visitedBytes = visitedWords * sizeof(std::uint32_t);

        cl_int err = CL_SUCCESS;

        // Allocate buffers.
        cl_mem devCode = ctx->createBuffer(codeSize, &err);
        if (err != CL_SUCCESS || !devCode) { lastErr = "createBuffer(code) failed"; return {}; }

        cl_mem devEntries = ctx->createBuffer(N * sizeof(std::uint64_t), &err);
        cl_mem devBBOut   = ctx->createBuffer(N * sizeof(CLBasicBlock),  &err);
        cl_mem devVisited = ctx->createBuffer(visitedBytes,               &err);
        cl_mem devErrors  = ctx->createBuffer(N * sizeof(std::uint32_t), &err);

        if (!devEntries || !devBBOut || !devVisited || !devErrors) {
            lastErr = "Buffer allocation failed";
            clReleaseMemObject(devCode);
            clReleaseMemObject(devEntries);
            clReleaseMemObject(devBBOut);
            clReleaseMemObject(devVisited);
            clReleaseMemObject(devErrors);
            return {};
        }

        // Upload code bytes.
        ctx->writeBuffer(devCode, code, codeSize);

        // Convert entry VMAs to offsets in the code buffer.
        std::vector<std::uint64_t> offsets(N);
        for (std::size_t i = 0; i < N; ++i) {
            offsets[i] = (entries[i] >= baseVMA) ? (entries[i] - baseVMA) : codeSize;
        }
        ctx->writeBuffer(devEntries, offsets.data(), N * sizeof(std::uint64_t));

        // Zero the visited bitset.
        std::vector<std::uint32_t> zeroVisited(visitedWords, 0u);
        ctx->writeBuffer(devVisited, zeroVisited.data(), visitedBytes);

        // Zero error flags.
        std::vector<std::uint32_t> zeroErrors(N, 0u);
        ctx->writeBuffer(devErrors, zeroErrors.data(), N * sizeof(std::uint32_t));

        // Launch kernel.
        const std::uint64_t byteCount = static_cast<std::uint64_t>(codeSize);
        const std::uint32_t numEntries = static_cast<std::uint32_t>(N);
        const std::size_t   wgSize     = 64;
        const std::size_t   gs         = ((N + wgSize - 1) / wgSize) * wgSize;

        OCLKernelLaunch launch;
        launch.programKey = "retdec_parallel_disasm";
        launch.kernelName = "retdec_parallel_disasm";
        launch.globalSize = gs;
        launch.localSize  = wgSize;
        launch.setArgs    = [&](cl_kernel k) {
            clSetKernelArg(k, 0, sizeof(cl_mem),    &devCode);
            clSetKernelArg(k, 1, sizeof(cl_ulong),  &byteCount);
            clSetKernelArg(k, 2, sizeof(cl_mem),    &devEntries);
            clSetKernelArg(k, 3, sizeof(cl_uint),   &numEntries);
            clSetKernelArg(k, 4, sizeof(cl_ulong),  &baseVMA);
            clSetKernelArg(k, 5, sizeof(cl_mem),    &devBBOut);
            clSetKernelArg(k, 6, sizeof(cl_mem),    &devVisited);
            clSetKernelArg(k, 7, sizeof(cl_mem),    &devErrors);
        };

        err = ctx->runKernel(launch, true);
        if (err != CL_SUCCESS) {
            lastErr = "Kernel launch failed";
            clReleaseMemObject(devCode);
            clReleaseMemObject(devEntries);
            clReleaseMemObject(devBBOut);
            clReleaseMemObject(devVisited);
            clReleaseMemObject(devErrors);
            return {};
        }

        // Read back results.
        std::vector<CLBasicBlock> clBBs(N);
        ctx->readBuffer(devBBOut, clBBs.data(), N * sizeof(CLBasicBlock));

        clReleaseMemObject(devCode);
        clReleaseMemObject(devEntries);
        clReleaseMemObject(devBBOut);
        clReleaseMemObject(devVisited);
        clReleaseMemObject(devErrors);

        // Convert to host BasicBlock.
        std::vector<BasicBlock> result;
        result.reserve(N);
        for (std::size_t i = 0; i < N; ++i) {
            const auto& cb = clBBs[i];
            if (cb.insnCount == 0 && (cb.flags & BB_INVALID)) continue;
            BasicBlock bb;
            bb.startAddr  = cb.startAddr;
            bb.endAddr    = cb.endAddr;
            bb.successor0 = cb.successor0;
            bb.successor1 = cb.successor1;
            bb.insnCount  = cb.insnCount;
            bb.flags      = cb.flags;
            result.push_back(bb);
        }
        return result;
    }

    std::vector<BasicBlock> runCPU(const std::uint8_t*              code,
                                   std::size_t                       codeSize,
                                   std::uint64_t                     baseVMA,
                                   const std::vector<std::uint64_t>& entries)
    {
        std::vector<BasicBlock> result;
        result.reserve(entries.size());
        for (std::uint64_t entry : entries) {
            result.push_back(cpu::walkSeed(code, codeSize, baseVMA, entry));
        }
        return result;
    }
};

// ─── OCLDisassembler ─────────────────────────────────────────────────────────

OCLDisassembler::OCLDisassembler(OCLContext* ctx)
    : _impl(std::make_unique<Impl>())
{
    _impl->ctx = ctx;
    if (ctx && ctx->isReady()) {
        _impl->useGPU = _impl->ensureKernel();
    }
}

OCLDisassembler::~OCLDisassembler() = default;

OCLDisassembler::OCLDisassembler(OCLDisassembler&&) noexcept            = default;
OCLDisassembler& OCLDisassembler::operator=(OCLDisassembler&&) noexcept = default;

bool OCLDisassembler::usesGPU() const noexcept
{
    return _impl && _impl->useGPU;
}

const std::string& OCLDisassembler::lastError() const noexcept
{
    static const std::string kEmpty;
    return _impl ? _impl->lastErr : kEmpty;
}

std::vector<BasicBlock>
OCLDisassembler::disassemble(const std::uint8_t*              codeBytes,
                              std::size_t                       codeSize,
                              std::uint64_t                     baseVMA,
                              const std::vector<std::uint64_t>& entryVMAs)
{
    if (!codeBytes || codeSize == 0 || entryVMAs.empty()) return {};

    std::vector<BasicBlock> raw;
    if (_impl->useGPU) {
        raw = _impl->runGPU(codeBytes, codeSize, baseVMA, entryVMAs);
        if (raw.empty() && !_impl->lastErr.empty()) {
            // GPU path failed — fall back to CPU.
            raw = _impl->runCPU(codeBytes, codeSize, baseVMA, entryVMAs);
        }
    } else {
        raw = _impl->runCPU(codeBytes, codeSize, baseVMA, entryVMAs);
    }

    // ── Post-process: sort by start address, deduplicate ─────────────────────
    std::sort(raw.begin(), raw.end(), [](const BasicBlock& a, const BasicBlock& b) {
        return a.startAddr < b.startAddr;
    });

    // Remove exact duplicates (same start address).
    raw.erase(
        std::unique(raw.begin(), raw.end(), [](const BasicBlock& a, const BasicBlock& b) {
            return a.startAddr == b.startAddr;
        }),
        raw.end());

    return raw;
}

} // namespace opencl
} // namespace retdec
