/**
 * @file src/utils/gpu_scanner.cu
 * @brief CUDA kernels for GPU-accelerated signature matching and entropy.
 * @copyright (c) 2024 RetDec contributors, MIT license
 *
 * Build requirements:
 *   - CUDA toolkit >= 11.0
 *   - Compute capability >= 6.0 (Pascal) recommended; 3.5 minimum
 *   - cmake: find_package(CUDAToolkit REQUIRED)
 *
 * Architecture:
 *   batchMatchKernel  — one CUDA block per signature pattern.
 *                       Each block slides the pattern across the file region
 *                       using shared memory to cache the pattern.
 *                       Threads within a block cooperate to scan positions.
 *
 *   entropyKernel     — standard parallel histogram + reduction.
 *                       256-bucket byte frequency, then log2 reduction.
 *
 *   findAllKernel     — one thread per candidate start position, checks
 *                       needle match using coalesced reads.
 */

#include <cuda_runtime.h>
#include <device_launch_parameters.h>

#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

#include "retdec/utils/gpu_scanner.h"

// ---------------------------------------------------------------------------
// Helpers / macros
// ---------------------------------------------------------------------------

#define CUDA_CHECK(call)                                                       \
    do {                                                                       \
        cudaError_t _e = (call);                                               \
        if (_e != cudaSuccess) {                                               \
            throw std::runtime_error(                                          \
                std::string("CUDA error in " #call ": ") +                    \
                cudaGetErrorString(_e));                                       \
        }                                                                      \
    } while (0)

namespace {

// ---------------------------------------------------------------------------
// CUDA kernels
// ---------------------------------------------------------------------------

/**
 * One block per pattern. Each thread within the block tests one candidate
 * file offset. Threads cooperate via shared memory to cache the pattern.
 *
 * Pattern nibble encoding (same as RetDec CPU path):
 *   hex char ['0'-'f'] = exact match required
 *   '?'  = wildcard (any nibble)
 *   '-'  = don't-care (same as '?')
 *   ';'  = pattern end marker
 *   '/'  = slashed jump (not handled on GPU; pre-filtered by host)
 *
 * Output per pattern: { matched, bestRatio, sameNibs, totalNibs, offset }
 * packed into a struct of arrays for coalescing.
 */

struct GpuMatchResult {
    uint32_t matched;    // 0 or 1
    float    bestRatio;
    uint32_t sameNibs;
    uint32_t totalNibs;
    uint32_t offset;     // byte offset of best match
};

// Maximum pattern length handled in shared memory.
static constexpr int MAX_PATTERN_NIBS = 4096;
// Threads per block for match kernel.
static constexpr int MATCH_BLOCK = 256;

__global__ void batchMatchKernel(
    const uint8_t* __restrict__ fileNibs,   // nibbles: '0'-'f', lowercase
    uint32_t                    fileNibLen,
    const char*  __restrict__   patterns,   // all patterns concatenated, '\0'-sep
    const uint32_t* __restrict__ patOffsets,// offset of each pattern in patterns[]
    const uint32_t* __restrict__ patLens,   // nibble length of each pattern
    uint32_t                    numPatterns,
    uint32_t                    scanStartNib,
    uint32_t                    scanEndNib,  // inclusive
    GpuMatchResult* __restrict__ results)
{
    const uint32_t pid = blockIdx.x;
    if (pid >= numPatterns) return;

    // Load pattern into shared memory.
    __shared__ char sPat[MAX_PATTERN_NIBS];
    const uint32_t patLen = patLens[pid];
    const char*    pat    = patterns + patOffsets[pid];

    for (uint32_t i = threadIdx.x; i < patLen; i += MATCH_BLOCK) {
        sPat[i] = pat[i];
    }
    __syncthreads();

    // Each thread tests one starting nibble position.
    const uint32_t endPos = (scanEndNib < fileNibLen)
                            ? scanEndNib : (fileNibLen - 1);
    const uint32_t maxStart = (endPos + 1 >= patLen) ? (endPos + 1 - patLen) : 0;

    uint32_t localBestSame  = 0;
    uint32_t localBestTotal = 0;
    float    localBestRatio = 0.0f;
    uint32_t localBestOff   = 0;
    uint32_t localMatched   = 0;

    for (uint32_t pos = scanStartNib + threadIdx.x;
         pos <= maxStart;
         pos += MATCH_BLOCK)
    {
        uint32_t same  = 0;
        uint32_t total = 0;

        for (uint32_t si = 0; si < patLen; ++si) {
            const char pc = sPat[si];
            if (pc == ';' || pc == '\0') break;
            if (pc == '?' || pc == '-') continue;
            // Wildcard / slashed already removed by host.
            ++total;
            const uint8_t fc = fileNibs[pos + si];
            // Compare nibble: fileNibs stores the raw nibble char.
            if ((uint8_t)pc == fc) ++same;
        }

        if (total > 0) {
            float ratio = (float)same / (float)total;
            if (ratio > localBestRatio ||
                (ratio == localBestRatio && total > localBestTotal))
            {
                localBestRatio = ratio;
                localBestSame  = same;
                localBestTotal = total;
                // Convert nibble offset to byte offset.
                localBestOff   = pos / 2;
                localMatched   = (ratio >= 0.5f) ? 1u : 0u;
            }
        }
    }

    // Block-level reduction: find global best across threads.
    __shared__ float    shRatio[MATCH_BLOCK];
    __shared__ uint32_t shSame[MATCH_BLOCK];
    __shared__ uint32_t shTotal[MATCH_BLOCK];
    __shared__ uint32_t shOff[MATCH_BLOCK];
    __shared__ uint32_t shMatched[MATCH_BLOCK];

    shRatio[threadIdx.x]   = localBestRatio;
    shSame[threadIdx.x]    = localBestSame;
    shTotal[threadIdx.x]   = localBestTotal;
    shOff[threadIdx.x]     = localBestOff;
    shMatched[threadIdx.x] = localMatched;
    __syncthreads();

    for (uint32_t stride = MATCH_BLOCK / 2; stride > 0; stride >>= 1) {
        if (threadIdx.x < stride) {
            if (shRatio[threadIdx.x + stride] > shRatio[threadIdx.x] ||
                (shRatio[threadIdx.x + stride] == shRatio[threadIdx.x] &&
                 shTotal[threadIdx.x + stride]  > shTotal[threadIdx.x]))
            {
                shRatio[threadIdx.x]   = shRatio[threadIdx.x + stride];
                shSame[threadIdx.x]    = shSame[threadIdx.x + stride];
                shTotal[threadIdx.x]   = shTotal[threadIdx.x + stride];
                shOff[threadIdx.x]     = shOff[threadIdx.x + stride];
                shMatched[threadIdx.x] = shMatched[threadIdx.x + stride];
            }
        }
        __syncthreads();
    }

    if (threadIdx.x == 0) {
        results[pid].matched    = shMatched[0];
        results[pid].bestRatio  = shRatio[0];
        results[pid].sameNibs   = shSame[0];
        results[pid].totalNibs  = shTotal[0];
        results[pid].offset     = shOff[0];
    }
}

// ---------------------------------------------------------------------------
// Entropy kernel
// ---------------------------------------------------------------------------

static constexpr int ENTROPY_BLOCK = 256;

__global__ void buildHistogramKernel(
    const uint8_t* __restrict__ data,
    uint32_t                    size,
    uint32_t* __restrict__      histogram)  // 256 buckets
{
    __shared__ uint32_t localHist[256];
    if (threadIdx.x < 256) localHist[threadIdx.x] = 0;
    __syncthreads();

    const uint32_t idx = blockIdx.x * blockDim.x + threadIdx.x;
    const uint32_t stride = gridDim.x * blockDim.x;

    for (uint32_t i = idx; i < size; i += stride) {
        atomicAdd(&localHist[data[i]], 1u);
    }
    __syncthreads();

    if (threadIdx.x < 256) {
        atomicAdd(&histogram[threadIdx.x], localHist[threadIdx.x]);
    }
}

__global__ void computeEntropyKernel(
    const uint32_t* __restrict__ histogram,
    uint32_t                     totalBytes,
    float* __restrict__          entropy)   // single output value
{
    __shared__ float partial[256];
    const uint32_t tid = threadIdx.x;

    float h = 0.0f;
    if (tid < 256 && histogram[tid] > 0) {
        float p = (float)histogram[tid] / (float)totalBytes;
        h = -p * log2f(p);
    }
    partial[tid] = h;
    __syncthreads();

    // Parallel reduce.
    for (uint32_t stride = 128; stride > 0; stride >>= 1) {
        if (tid < stride) partial[tid] += partial[tid + stride];
        __syncthreads();
    }
    if (tid == 0) *entropy = partial[0];
}

// ---------------------------------------------------------------------------
// findAll kernel
// ---------------------------------------------------------------------------

__global__ void findAllKernel(
    const uint8_t* __restrict__ data,
    uint32_t                    dataSize,
    const uint8_t* __restrict__ needle,
    uint32_t                    needleLen,
    uint32_t* __restrict__      matchFlags)  // 1 per candidate position
{
    const uint32_t pos = blockIdx.x * blockDim.x + threadIdx.x;
    if (pos + needleLen > dataSize) return;

    bool ok = true;
    for (uint32_t i = 0; i < needleLen && ok; ++i) {
        ok = (data[pos + i] == needle[i]);
    }
    matchFlags[pos] = ok ? 1u : 0u;
}

// ---------------------------------------------------------------------------
// Host-side Impl
// ---------------------------------------------------------------------------

} // anonymous namespace

namespace retdec {
namespace utils {

struct GpuScanner::Impl {
    bool   gpuAvailable = false;
    int    deviceId     = -1;
    char   deviceNameStr[256] = "CPU fallback";

    // Device memory for the uploaded file.
    uint8_t*  d_fileBytes = nullptr;   // raw bytes
    uint8_t*  d_fileNibs  = nullptr;   // nibble chars ('0'-'f')
    uint32_t  fileSize    = 0;
    uint32_t  fileNibLen  = 0;

    // Host-side nibble string (for CPU fallback).
    std::vector<uint8_t> h_fileBytes;
    std::string          h_fileNibs;

    void ensureGpu() {
        int count = 0;
        if (cudaGetDeviceCount(&count) != cudaSuccess || count == 0) return;
        // Pick device 0 - could be extended to pick by P920 topology.
        if (cudaSetDevice(0) != cudaSuccess) return;
        cudaDeviceProp prop{};
        if (cudaGetDeviceProperties(&prop, 0) != cudaSuccess) return;
        strncpy(deviceNameStr, prop.name, sizeof(deviceNameStr) - 1);
        deviceId    = 0;
        gpuAvailable = true;
    }

    void freeDeviceFile() {
        if (d_fileBytes) { cudaFree(d_fileBytes); d_fileBytes = nullptr; }
        if (d_fileNibs)  { cudaFree(d_fileNibs);  d_fileNibs  = nullptr; }
        fileSize   = 0;
        fileNibLen = 0;
    }

    // Convert bytes to nibble chars on the host and upload both to GPU.
    void uploadToGpu(const uint8_t* data, std::size_t size) {
        static const char hexLut[] = "0123456789abcdef";
        std::string nibs;
        nibs.resize(size * 2);
        for (std::size_t i = 0; i < size; ++i) {
            nibs[i * 2]     = hexLut[data[i] >> 4];
            nibs[i * 2 + 1] = hexLut[data[i] & 0xF];
        }

        CUDA_CHECK(cudaMalloc(&d_fileBytes, size));
        CUDA_CHECK(cudaMemcpy(d_fileBytes, data, size, cudaMemcpyHostToDevice));

        const std::size_t nibBytes = nibs.size();
        CUDA_CHECK(cudaMalloc(&d_fileNibs, nibBytes));
        CUDA_CHECK(cudaMemcpy(d_fileNibs, nibs.data(), nibBytes, cudaMemcpyHostToDevice));

        fileSize   = static_cast<uint32_t>(size);
        fileNibLen = static_cast<uint32_t>(nibBytes);
    }

    ~Impl() { freeDeviceFile(); }
};

// ---------------------------------------------------------------------------
// Public interface
// ---------------------------------------------------------------------------

GpuScanner::GpuScanner() : impl_(new Impl()) {
    impl_->ensureGpu();
}

GpuScanner::~GpuScanner() { delete impl_; }

bool GpuScanner::isAvailable() const { return impl_->gpuAvailable; }

std::string GpuScanner::deviceName() const { return impl_->deviceNameStr; }

void GpuScanner::uploadFile(const uint8_t* data, std::size_t size) {
    // Always keep host copy for CPU fallback paths.
    impl_->h_fileBytes.assign(data, data + size);
    static const char hexLut[] = "0123456789abcdef";
    impl_->h_fileNibs.resize(size * 2);
    for (std::size_t i = 0; i < size; ++i) {
        impl_->h_fileNibs[i * 2]     = hexLut[data[i] >> 4];
        impl_->h_fileNibs[i * 2 + 1] = hexLut[data[i] & 0xF];
    }

    if (!impl_->gpuAvailable) return;
    impl_->freeDeviceFile();
    impl_->uploadToGpu(data, size);
}

// ---------------------------------------------------------------------------
// batchMatch — GPU path
// ---------------------------------------------------------------------------

static SigMatchResult cpuMatchOne(
    const std::string& nibs,
    const std::string& pat,
    std::size_t startNib,
    std::size_t endNib)
{
    SigMatchResult r;
    const std::size_t patLen = pat.find(';') != std::string::npos
                               ? pat.find(';') : pat.size();
    if (patLen == 0 || endNib < patLen) return r;
    const std::size_t maxStart = endNib - patLen + 1;

    for (std::size_t pos = startNib; pos <= maxStart; ++pos) {
        uint32_t same = 0, total = 0;
        for (std::size_t si = 0; si < patLen; ++si) {
            char pc = pat[si];
            if (pc == ';' || pc == '\0') break;
            if (pc == '?' || pc == '-') continue;
            ++total;
            if ((uint8_t)pc == (uint8_t)nibs[pos + si]) ++same;
        }
        if (total > 0) {
            double ratio = (double)same / (double)total;
            if (ratio > r.bestRatio ||
                (ratio == r.bestRatio && total > r.totalNibs))
            {
                r.bestRatio  = ratio;
                r.sameNibs   = same;
                r.totalNibs  = total;
                r.offset     = static_cast<uint32_t>(pos / 2);
                r.matched    = ratio >= 0.5;
            }
        }
    }
    return r;
}

std::vector<SigMatchResult> GpuScanner::batchMatch(
    const std::vector<std::string>& patterns,
    std::size_t startOffset,
    std::size_t stopOffset) const
{
    const std::size_t n = patterns.size();
    std::vector<SigMatchResult> results(n);
    if (n == 0 || impl_->h_fileNibs.empty()) return results;

    const std::size_t fileNibLen = impl_->h_fileNibs.size();
    const std::size_t startNib   = startOffset * 2;
    const std::size_t endNib     = (stopOffset == SIZE_MAX)
                                   ? fileNibLen - 1
                                   : std::min(stopOffset * 2 + 1, fileNibLen - 1);

    // Separate GPU-friendly (no '/') and CPU-only (has '/') patterns.
    std::vector<std::size_t> gpuIdx, cpuIdx;
    for (std::size_t i = 0; i < n; ++i) {
        if (patterns[i].find('/') != std::string::npos)
            cpuIdx.push_back(i);
        else
            gpuIdx.push_back(i);
    }

    // CPU path for slashed patterns (small fraction in practice).
    for (std::size_t i : cpuIdx) {
        results[i] = cpuMatchOne(impl_->h_fileNibs, patterns[i], startNib, endNib);
    }

    if (!impl_->gpuAvailable || gpuIdx.empty()) {
        // Full CPU fallback.
        for (std::size_t i : gpuIdx) {
            results[i] = cpuMatchOne(impl_->h_fileNibs, patterns[i], startNib, endNib);
        }
        return results;
    }

    // --- GPU path ---
    // Pack patterns into a flat buffer.
    std::string  patBuf;
    std::vector<uint32_t> patOffsets(gpuIdx.size());
    std::vector<uint32_t> patLens(gpuIdx.size());

    for (std::size_t gi = 0; gi < gpuIdx.size(); ++gi) {
        patOffsets[gi] = static_cast<uint32_t>(patBuf.size());
        const auto& p  = patterns[gpuIdx[gi]];
        // Truncate at ';'
        const auto  ep = p.find(';');
        const std::string pp = (ep != std::string::npos) ? p.substr(0, ep) : p;
        patLens[gi] = static_cast<uint32_t>(pp.size());
        patBuf += pp;
    }

    const uint32_t numGpu = static_cast<uint32_t>(gpuIdx.size());

    char*     d_patBuf    = nullptr;
    uint32_t* d_offsets   = nullptr;
    uint32_t* d_lens      = nullptr;
    GpuMatchResult* d_res = nullptr;

    try {
        CUDA_CHECK(cudaMalloc(&d_patBuf,  patBuf.size()));
        CUDA_CHECK(cudaMalloc(&d_offsets, numGpu * sizeof(uint32_t)));
        CUDA_CHECK(cudaMalloc(&d_lens,    numGpu * sizeof(uint32_t)));
        CUDA_CHECK(cudaMalloc(&d_res,     numGpu * sizeof(GpuMatchResult)));

        CUDA_CHECK(cudaMemcpy(d_patBuf,  patBuf.data(),    patBuf.size(),           cudaMemcpyHostToDevice));
        CUDA_CHECK(cudaMemcpy(d_offsets, patOffsets.data(), numGpu*sizeof(uint32_t), cudaMemcpyHostToDevice));
        CUDA_CHECK(cudaMemcpy(d_lens,    patLens.data(),   numGpu*sizeof(uint32_t), cudaMemcpyHostToDevice));

        // Zero results.
        CUDA_CHECK(cudaMemset(d_res, 0, numGpu * sizeof(GpuMatchResult)));

        batchMatchKernel<<<numGpu, MATCH_BLOCK>>>(
            impl_->d_fileNibs,
            impl_->fileNibLen,
            d_patBuf,
            d_offsets,
            d_lens,
            numGpu,
            static_cast<uint32_t>(startNib),
            static_cast<uint32_t>(endNib),
            d_res
        );
        CUDA_CHECK(cudaGetLastError());
        CUDA_CHECK(cudaDeviceSynchronize());

        // Copy results back.
        std::vector<GpuMatchResult> h_res(numGpu);
        CUDA_CHECK(cudaMemcpy(h_res.data(), d_res,
                              numGpu * sizeof(GpuMatchResult),
                              cudaMemcpyDeviceToHost));

        for (std::size_t gi = 0; gi < gpuIdx.size(); ++gi) {
            auto& r = results[gpuIdx[gi]];
            r.matched    = h_res[gi].matched != 0;
            r.bestRatio  = h_res[gi].bestRatio;
            r.sameNibs   = h_res[gi].sameNibs;
            r.totalNibs  = h_res[gi].totalNibs;
            r.offset     = h_res[gi].offset;
        }
    } catch (...) {
        // GPU error — fall back to CPU for remaining patterns.
        for (std::size_t gi = 0; gi < gpuIdx.size(); ++gi) {
            results[gpuIdx[gi]] = cpuMatchOne(
                impl_->h_fileNibs, patterns[gpuIdx[gi]], startNib, endNib);
        }
    }

    if (d_patBuf)  cudaFree(d_patBuf);
    if (d_offsets) cudaFree(d_offsets);
    if (d_lens)    cudaFree(d_lens);
    if (d_res)     cudaFree(d_res);

    return results;
}

// ---------------------------------------------------------------------------
// fileEntropy
// ---------------------------------------------------------------------------

double GpuScanner::fileEntropy(std::size_t startOffset, std::size_t stopOffset) const {
    const auto& bytes = impl_->h_fileBytes;
    if (bytes.empty()) return 0.0;

    const std::size_t lo = startOffset;
    const std::size_t hi = (stopOffset == SIZE_MAX) ? bytes.size() - 1
                                                    : std::min(stopOffset, bytes.size() - 1);
    const std::size_t sz = hi - lo + 1;

    if (!impl_->gpuAvailable) {
        // CPU fallback: plain histogram.
        uint32_t hist[256] = {};
        for (std::size_t i = lo; i <= hi; ++i) hist[bytes[i]]++;
        double e = 0.0;
        for (int b = 0; b < 256; ++b) {
            if (hist[b] == 0) continue;
            double p = (double)hist[b] / (double)sz;
            e -= p * std::log2(p);
        }
        return e;
    }

    uint32_t* d_hist  = nullptr;
    float*    d_entr  = nullptr;
    float     h_entr  = 0.0f;

    try {
        CUDA_CHECK(cudaMalloc(&d_hist, 256 * sizeof(uint32_t)));
        CUDA_CHECK(cudaMalloc(&d_entr, sizeof(float)));
        CUDA_CHECK(cudaMemset(d_hist, 0, 256 * sizeof(uint32_t)));

        const int blocks = static_cast<int>((sz + ENTROPY_BLOCK - 1) / ENTROPY_BLOCK);
        buildHistogramKernel<<<blocks, ENTROPY_BLOCK>>>(
            impl_->d_fileBytes + lo,
            static_cast<uint32_t>(sz),
            d_hist
        );
        CUDA_CHECK(cudaGetLastError());

        computeEntropyKernel<<<1, 256>>>(
            d_hist,
            static_cast<uint32_t>(sz),
            d_entr
        );
        CUDA_CHECK(cudaGetLastError());
        CUDA_CHECK(cudaDeviceSynchronize());
        CUDA_CHECK(cudaMemcpy(&h_entr, d_entr, sizeof(float), cudaMemcpyDeviceToHost));
    } catch (...) {
        // CPU fallback on error.
        uint32_t hist[256] = {};
        for (std::size_t i = lo; i <= hi; ++i) hist[bytes[i]]++;
        double e = 0.0;
        for (int b = 0; b < 256; ++b) {
            if (!hist[b]) continue;
            double p = (double)hist[b] / (double)sz;
            e -= p * std::log2(p);
        }
        if (d_hist) cudaFree(d_hist);
        if (d_entr) cudaFree(d_entr);
        return e;
    }

    cudaFree(d_hist);
    cudaFree(d_entr);
    return static_cast<double>(h_entr);
}

// ---------------------------------------------------------------------------
// findAll
// ---------------------------------------------------------------------------

std::vector<std::size_t> GpuScanner::findAll(const std::vector<uint8_t>& needle) const {
    std::vector<std::size_t> offsets;
    const auto& bytes = impl_->h_fileBytes;
    if (bytes.empty() || needle.empty() || needle.size() > bytes.size())
        return offsets;

    if (!impl_->gpuAvailable) {
        // CPU fallback.
        const uint8_t* b = bytes.data();
        const uint8_t* e = b + bytes.size();
        const uint8_t* n = needle.data();
        for (auto it = b; (it = std::search(it, e, n, n + needle.size())) != e; ++it)
            offsets.push_back(static_cast<std::size_t>(it - b));
        return offsets;
    }

    const uint32_t numPositions = static_cast<uint32_t>(
        bytes.size() - needle.size() + 1);

    uint8_t*  d_needle = nullptr;
    uint32_t* d_flags  = nullptr;

    try {
        CUDA_CHECK(cudaMalloc(&d_needle, needle.size()));
        CUDA_CHECK(cudaMalloc(&d_flags,  numPositions * sizeof(uint32_t)));
        CUDA_CHECK(cudaMemcpy(d_needle, needle.data(), needle.size(), cudaMemcpyHostToDevice));
        CUDA_CHECK(cudaMemset(d_flags,  0, numPositions * sizeof(uint32_t)));

        const int blk    = 256;
        const int blocks = (numPositions + blk - 1) / blk;
        findAllKernel<<<blocks, blk>>>(
            impl_->d_fileBytes,
            impl_->fileSize,
            d_needle,
            static_cast<uint32_t>(needle.size()),
            d_flags
        );
        CUDA_CHECK(cudaGetLastError());
        CUDA_CHECK(cudaDeviceSynchronize());

        std::vector<uint32_t> h_flags(numPositions);
        CUDA_CHECK(cudaMemcpy(h_flags.data(), d_flags,
                              numPositions * sizeof(uint32_t),
                              cudaMemcpyDeviceToHost));
        for (uint32_t i = 0; i < numPositions; ++i)
            if (h_flags[i]) offsets.push_back(i);
    } catch (...) {
        // CPU fallback.
        const uint8_t* b = bytes.data();
        const uint8_t* e = b + bytes.size();
        const uint8_t* n = needle.data();
        for (auto it = b; (it = std::search(it, e, n, n + needle.size())) != e; ++it)
            offsets.push_back(static_cast<std::size_t>(it - b));
    }

    if (d_needle) cudaFree(d_needle);
    if (d_flags)  cudaFree(d_flags);
    return offsets;
}

} // namespace utils
} // namespace retdec
