/**
 * @file include/retdec/cuda_accel/cuda_semantic_hasher.h
 * @brief CUDA-accelerated semantic hashing via mini x86 emulator — replaces OCLSemanticHasher.
 */
#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace retdec::cuda_accel {

class CUDAContext;

static constexpr std::uint32_t kTestVectorCount = 16;
static constexpr std::uint32_t kTestInputWidth  = 8;

using TestInputMatrix = std::uint64_t[kTestVectorCount][kTestInputWidth];

struct FunctionBytecode {
    std::vector<std::uint8_t> bytes;
    std::uint64_t             baseVMA{0};
};

enum class EmulationStatus : std::uint32_t {
    OK          = 0,
    Halted      = 1,
    Fault       = 2,
    StepLimit   = 3,
    Unsupported = 4,
};

struct IOSignature {
    std::uint64_t   ioHash{0};
    EmulationStatus status{EmulationStatus::OK};
};

std::vector<std::array<std::uint64_t, kTestInputWidth>>
defaultTestVectors(std::uint32_t funcIdx = 0);

class CUDASemanticHasher {
public:
    static constexpr std::uint32_t kScratchBytesPerWI = 4096u;

    explicit CUDASemanticHasher(CUDAContext* ctx = nullptr);
    ~CUDASemanticHasher();

    std::vector<IOSignature> hash(const std::vector<FunctionBytecode>& funcs);

    bool               usesGPU()   const noexcept { return ctx_ && gpuReady_; }
    const std::string& lastError() const noexcept { return lastError_; }

private:
    std::vector<IOSignature> hashCPU(const std::vector<FunctionBytecode>& funcs);

    CUDAContext* ctx_{nullptr};
    bool         gpuReady_{false};
    std::string  lastError_;
};

class SemanticHashDB {
public:
    void        insert(std::string name, std::uint64_t ioHash);
    std::string lookup(std::uint64_t ioHash) const;
    std::size_t size() const noexcept { return entries_.size(); }

private:
    struct Entry { std::string name; std::uint64_t hash; };
    std::vector<Entry> entries_;
};

} // namespace retdec::cuda_accel
