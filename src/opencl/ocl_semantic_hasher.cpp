/**
 * @file src/opencl/ocl_semantic_hasher.cpp
 * @brief OCLSemanticHasher + SemanticHashDB implementation.
 */

#include <memory>
#include "retdec/opencl/ocl_semantic_hasher.h"
#include "retdec/opencl/ocl_context.h"
#include "retdec/opencl/kernel_sources.h"

#include <CL/cl.h>

#include <algorithm>
#include <cstring>
#include <vector>

namespace retdec {
namespace opencl {

// ─── Default adversarial test vectors ────────────────────────────────────────

TestInputMatrix defaultTestVectors(std::uint32_t funcIdx)
{
    TestInputMatrix m{};

    // Each row: [RDI, RSI, RDX, RCX, R8, R9, RAX, pad]
    // We use pseudo-random but reproducible values that cover edge cases:
    //   - 0, 1, small positives/negatives, large values, max/min
    static const std::uint64_t base[kTestInputWidth] = {
        0x0000000000000000ULL,
        0x0000000000000001ULL,
        0x00000000FFFFFFFFULL,
        0x7FFFFFFFFFFFFFFFULL,
        0x8000000000000000ULL,
        0xFFFFFFFFFFFFFFFFULL,
        0x123456789ABCDEF0ULL,
        0xDEADBEEFCAFEBABEULL,
    };

    for (std::size_t tv = 0; tv < kTestVectorCount; ++tv) {
        for (std::size_t i = 0; i < kTestInputWidth; ++i) {
            // Mix with funcIdx and tv to get function-specific adversarial vectors
            std::uint64_t x = base[i % kTestInputWidth];
            x ^= (static_cast<std::uint64_t>(funcIdx) * 0x9e3779b97f4a7c15ULL);
            x ^= (static_cast<std::uint64_t>(tv + 1)  * 0x6c62272e07bb0142ULL * (i + 1));
            m[tv][i] = x;
        }
    }
    // Always include all-zero and all-one test vectors.
    m[0].fill(0);
    m[1].fill(0xFFFFFFFFFFFFFFFFULL);
    m[2].fill(1);

    return m;
}

// ─── CPU path: emulate function on host ──────────────────────────────────────
// A simplified host-side x86 emulator (mirrors the kernel for CPU fallback)
namespace host_emu {

static constexpr std::uint32_t SCRATCH_SIZE = 4096;
static constexpr std::uint32_t MAX_STEPS    = 16384;

struct State {
    std::uint64_t regs[16] = {};
    std::uint32_t flags    = 0;
    std::uint32_t rip      = 0;
    std::uint32_t halted   = 0;
    std::uint8_t  scratch[SCRATCH_SIZE] = {};

    enum { CF=1, ZF=64, SF=128, OF=2048 };

    std::uint64_t readMem(std::uint32_t addr) const {
        if (addr + 8 > SCRATCH_SIZE) return 0;
        std::uint64_t v = 0;
        for (int i = 0; i < 8; ++i) v |= (std::uint64_t)scratch[addr+i] << (i*8);
        return v;
    }
    void writeMem(std::uint32_t addr, std::uint64_t val) {
        if (addr + 8 > SCRATCH_SIZE) return;
        for (int i = 0; i < 8; ++i) scratch[addr+i] = (std::uint8_t)(val >> (i*8));
    }
};

static std::uint64_t fnv1a(std::uint64_t h, std::uint64_t v) {
    for (int b = 0; b < 8; ++b) {
        h ^= (v & 0xFF); h *= 1099511628211ULL; v >>= 8;
    }
    return h;
}

static bool evalCond(std::uint32_t flags, std::uint32_t c) {
    bool cf = (flags & State::CF) != 0, zf = (flags & State::ZF) != 0;
    bool sf = (flags & State::SF) != 0, of = (flags & State::OF) != 0;
    switch (c & 0xF) {
    case 0: return of;   case 1: return !of;
    case 2: return cf;   case 3: return !cf;
    case 4: return zf;   case 5: return !zf;
    case 6: return cf||zf; case 7: return !cf&&!zf;
    case 8: return sf;   case 9: return !sf;
    case 0xA: return of != sf; case 0xB: return of == sf;
    case 0xC: return zf || (of != sf); case 0xD: return !zf && (of == sf);
    default: return false;
    }
}

static IOSignature emulate(const std::uint8_t* func, std::uint32_t funcSize,
                            const std::array<std::uint64_t, kTestInputWidth>& inputs)
{
    State st;
    // RSP points to top of scratch.
    st.regs[4] = SCRATCH_SIZE - 128; // RSP
    // Load calling convention.
    st.regs[7] = inputs[0]; // RDI
    st.regs[6] = inputs[1]; // RSI
    st.regs[2] = inputs[2]; // RDX
    st.regs[1] = inputs[3]; // RCX
    st.regs[8] = inputs[4]; // R8
    st.regs[9] = inputs[5]; // R9
    st.regs[0] = inputs[6]; // RAX

    for (std::uint32_t step = 0; step < MAX_STEPS && !st.halted && st.rip < funcSize; ++step) {
        std::uint32_t ip = st.rip;
        if (ip >= funcSize) break;

        bool rex_w = false, rex_r = false, rex_b = false;
        std::uint8_t b;

        // Skip prefixes
        while (ip < funcSize) {
            b = func[ip];
            if (b == 0x66 || b == 0x67 || b == 0xF0 || b == 0xF2 || b == 0xF3 ||
                b == 0x26 || b == 0x2E || b == 0x36 || b == 0x3E || b == 0x64 || b == 0x65) { ++ip; }
            else break;
        }
        if (ip >= funcSize) break;
        b = func[ip];
        if (b >= 0x40 && b <= 0x4F) { rex_w=(b&8)!=0; rex_r=(b&4)!=0; rex_b=(b&1)!=0; ++ip; }
        if (ip >= funcSize) break;

        std::uint8_t opc = func[ip++];

        auto readImm8  = [&]() -> std::int64_t { return ip < funcSize ? (std::int64_t)(std::int8_t)func[ip++] : 0; };
        auto readImm32 = [&]() -> std::int64_t {
            if (ip + 3 >= funcSize) { ip += 4; return 0; }
            std::int32_t v = (std::int32_t)((std::uint32_t)func[ip] | ((std::uint32_t)func[ip+1]<<8) |
                                             ((std::uint32_t)func[ip+2]<<16) | ((std::uint32_t)func[ip+3]<<24));
            ip += 4; return (std::int64_t)v;
        };

        if (opc == 0x90) { st.rip = ip; continue; }
        if (opc == 0xC3 || opc == 0xCB) { st.halted = 1; st.rip = ip; break; }
        if (opc == 0xE9) { auto r = readImm32(); st.rip = (std::uint32_t)((std::int32_t)ip + (std::int32_t)r); continue; }
        if (opc == 0xEB) { auto r = readImm8(); st.rip = (std::uint32_t)((std::int32_t)ip + (std::int32_t)r); continue; }
        if (opc >= 0x70 && opc <= 0x7F) {
            auto r = readImm8();
            st.rip = evalCond(st.flags, opc & 0xF) ? (std::uint32_t)((std::int32_t)ip+(std::int32_t)r) : ip;
            continue;
        }
        if (opc >= 0x50 && opc <= 0x57) {
            std::uint32_t reg = (opc&7) + (rex_b?8:0);
            st.regs[4] -= 8;
            st.writeMem((std::uint32_t)st.regs[4], st.regs[reg]);
            st.rip = ip; continue;
        }
        if (opc >= 0x58 && opc <= 0x5F) {
            std::uint32_t reg = (opc&7) + (rex_b?8:0);
            st.regs[reg] = st.readMem((std::uint32_t)st.regs[4]);
            st.regs[4] += 8;
            st.rip = ip; continue;
        }
        if (opc >= 0xB8 && opc <= 0xBF) {
            std::uint32_t reg = (opc&7) + (rex_b?8:0);
            if (rex_w) {
                std::uint64_t v = 0;
                for (int bb = 0; bb < 8 && ip+bb < funcSize; ++bb) v |= (std::uint64_t)func[ip+bb]<<(bb*8);
                ip += 8; st.regs[reg] = v;
            } else {
                st.regs[reg] = (std::uint64_t)(std::uint32_t)((ip+3<funcSize) ?
                    ((std::uint32_t)func[ip]|(std::uint32_t)func[ip+1]<<8|(std::uint32_t)func[ip+2]<<16|(std::uint32_t)func[ip+3]<<24) : 0);
                ip += 4;
            }
            st.rip = ip; continue;
        }
        // Fall through all others
        // Use length decoder to skip
        // Minimal implementation: find instruction length and skip
        std::uint32_t remStart = st.rip;
        std::uint32_t remLen = funcSize - remStart;
        // Very simplified skip for unknown instructions
        st.rip = ip;
        continue;
    }

    // Compute IO hash
    std::uint64_t h = 14695981039346656037ULL;
    for (std::size_t i = 0; i < 8; ++i) h = fnv1a(h, inputs[i]);
    h = fnv1a(h, st.regs[0]); // RAX
    h = fnv1a(h, st.regs[1]); // RCX
    h = fnv1a(h, st.regs[2]); // RDX
    h = fnv1a(h, st.regs[6]); // RSI
    h = fnv1a(h, st.regs[7]); // RDI

    IOSignature sig;
    sig.ioHash = h;
    sig.status = static_cast<EmulationStatus>(st.halted);
    return sig;
}

} // namespace host_emu

// ─── Impl ─────────────────────────────────────────────────────────────────────

struct OCLSemanticHasher::Impl {
    OCLContext* ctx    = nullptr;
    bool        useGPU = false;
    std::string lastErr;

    bool ensureKernel()
    {
        if (!ctx || !ctx->isReady()) return false;
        std::string log;
        bool ok = ctx->ensureProgram(
            "retdec_semantic_hash",
            semanticHashClSource(),
            "",
            &log);
        if (!ok) { lastErr = "semantic_hash build failed: " + log; }
        return ok;
    }

    std::vector<IOSignature> runCPU(const std::vector<FunctionBytecode>& funcs)
    {
        std::vector<IOSignature> result;
        result.reserve(funcs.size() * kTestVectorCount);
        for (const auto& f : funcs) {
            for (std::size_t tv = 0; tv < kTestVectorCount; ++tv) {
                result.push_back(host_emu::emulate(
                    f.bytes.data(),
                    static_cast<std::uint32_t>(f.bytes.size()),
                    f.testInputs[tv]));
            }
        }
        return result;
    }

    std::vector<IOSignature> runGPU(const std::vector<FunctionBytecode>& funcs)
    {
        const std::size_t numFuncs = funcs.size();
        const std::size_t numWI   = numFuncs * kTestVectorCount;

        // Build flat arrays.
        std::vector<std::uint8_t>  allBytes;
        std::vector<std::uint32_t> offsets(numWI), sizes(numWI);
        std::vector<std::uint64_t> inputs(numWI * kTestInputWidth);

        std::uint32_t byteOff = 0;
        for (std::size_t fi = 0; fi < numFuncs; ++fi) {
            std::uint32_t fsize = static_cast<std::uint32_t>(funcs[fi].bytes.size());
            allBytes.insert(allBytes.end(),
                            funcs[fi].bytes.begin(), funcs[fi].bytes.end());

            for (std::size_t tv = 0; tv < kTestVectorCount; ++tv) {
                std::size_t wi = fi * kTestVectorCount + tv;
                offsets[wi] = byteOff;
                sizes[wi]   = fsize;
                for (std::size_t i = 0; i < kTestInputWidth; ++i) {
                    inputs[wi * kTestInputWidth + i] = funcs[fi].testInputs[tv][i];
                }
            }
            byteOff += fsize;
        }

        if (allBytes.empty()) { allBytes.push_back(0xC3); } // dummy RET

        cl_int err = CL_SUCCESS;
        auto mkBuf = [&](std::size_t n) { return ctx->createBuffer(n ? n : 4, &err); };

        cl_mem dBytes   = mkBuf(allBytes.size());
        cl_mem dOff     = mkBuf(numWI * sizeof(std::uint32_t));
        cl_mem dSz      = mkBuf(numWI * sizeof(std::uint32_t));
        cl_mem dInputs  = mkBuf(numWI * kTestInputWidth * sizeof(std::uint64_t));
        cl_mem dScratch = mkBuf(numWI * kScratchBytesPerWI);
        cl_mem dHashes  = mkBuf(numWI * sizeof(std::uint64_t));
        cl_mem dStatus  = mkBuf(numWI * sizeof(std::uint32_t));

        if (!dBytes || !dOff || !dSz || !dInputs || !dScratch || !dHashes || !dStatus) {
            lastErr = "semantic_hash: GPU buffer alloc failed";
            for (cl_mem m : {dBytes,dOff,dSz,dInputs,dScratch,dHashes,dStatus}) if (m) clReleaseMemObject(m);
            return {};
        }

        ctx->writeBuffer(dBytes,  allBytes.data(), allBytes.size());
        ctx->writeBuffer(dOff,    offsets.data(),  numWI * sizeof(std::uint32_t));
        ctx->writeBuffer(dSz,     sizes.data(),    numWI * sizeof(std::uint32_t));
        ctx->writeBuffer(dInputs, inputs.data(),   numWI * kTestInputWidth * sizeof(std::uint64_t));

        // Zero scratch memory.
        std::vector<std::uint8_t> zeros(numWI * kScratchBytesPerWI, 0u);
        ctx->writeBuffer(dScratch, zeros.data(), zeros.size());

        OCLKernelLaunch k;
        k.programKey = "retdec_semantic_hash";
        k.kernelName = "retdec_semantic_hash";
        k.globalSize = numWI;
        k.localSize  = 0;
        k.setArgs = [&](cl_kernel kk) {
            clSetKernelArg(kk, 0, sizeof(cl_mem), &dBytes);
            clSetKernelArg(kk, 1, sizeof(cl_mem), &dOff);
            clSetKernelArg(kk, 2, sizeof(cl_mem), &dSz);
            clSetKernelArg(kk, 3, sizeof(cl_mem), &dInputs);
            clSetKernelArg(kk, 4, sizeof(cl_mem), &dScratch);
            clSetKernelArg(kk, 5, sizeof(cl_mem), &dHashes);
            clSetKernelArg(kk, 6, sizeof(cl_mem), &dStatus);
        };
        ctx->runKernel(k, true);

        std::vector<std::uint64_t> hashes(numWI);
        std::vector<std::uint32_t> status(numWI);
        ctx->readBuffer(dHashes, hashes.data(), numWI * sizeof(std::uint64_t));
        ctx->readBuffer(dStatus, status.data(), numWI * sizeof(std::uint32_t));

        for (cl_mem m : {dBytes,dOff,dSz,dInputs,dScratch,dHashes,dStatus}) clReleaseMemObject(m);

        std::vector<IOSignature> result(numWI);
        for (std::size_t i = 0; i < numWI; ++i) {
            result[i].ioHash = hashes[i];
            result[i].status = static_cast<EmulationStatus>(status[i]);
        }
        return result;
    }
};

// ─── OCLSemanticHasher ────────────────────────────────────────────────────────

OCLSemanticHasher::OCLSemanticHasher(OCLContext* ctx)
    : _impl(std::make_unique<Impl>())
{
    _impl->ctx = ctx;
    if (ctx && ctx->isReady()) {
        _impl->useGPU = _impl->ensureKernel();
    }
}

OCLSemanticHasher::~OCLSemanticHasher()                                         = default;
OCLSemanticHasher::OCLSemanticHasher(OCLSemanticHasher&&) noexcept              = default;
OCLSemanticHasher& OCLSemanticHasher::operator=(OCLSemanticHasher&&) noexcept   = default;

bool OCLSemanticHasher::usesGPU() const noexcept { return _impl && _impl->useGPU; }
const std::string& OCLSemanticHasher::lastError() const noexcept {
    static const std::string kE; return _impl ? _impl->lastErr : kE;
}

std::vector<IOSignature>
OCLSemanticHasher::hash(const std::vector<FunctionBytecode>& funcs)
{
    if (funcs.empty()) return {};
    if (_impl->useGPU) {
        auto r = _impl->runGPU(funcs);
        if (!r.empty()) return r;
    }
    return _impl->runCPU(funcs);
}

// ─── SemanticHashDB ──────────────────────────────────────────────────────────

void SemanticHashDB::insert(std::string name, std::uint64_t ioHash)
{
    _entries.push_back({std::move(name), ioHash});
}

std::string SemanticHashDB::lookup(std::uint64_t ioHash) const
{
    for (const auto& e : _entries) {
        if (e.ioHash == ioHash) return e.name;
    }
    return {};
}

std::size_t SemanticHashDB::size() const noexcept { return _entries.size(); }

} // namespace opencl
} // namespace retdec
