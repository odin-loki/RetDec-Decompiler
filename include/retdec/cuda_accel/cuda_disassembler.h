/**
 * @file include/retdec/cuda_accel/cuda_disassembler.h
 * @brief Parallel x86-64 CFG disassembler — replaces OCLDisassembler.
 *
 * When CUDA is available each entry-point seed runs in a separate GPU thread.
 * Falls back to a CPU multi-threaded implementation using std::async.
 */
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace retdec::cuda_accel {

class CUDAContext;

static constexpr std::uint64_t kBBAddrNone = 0xFFFFFFFFFFFFFFFFULL;

enum BasicBlockFlags : std::uint32_t {
    BB_HAS_CALL  = 1u,
    BB_ENDS_RET  = 2u,
    BB_ENDS_JMP  = 4u,
    BB_ENDS_JCC  = 8u,
    BB_INVALID   = 16u,
};

struct BasicBlock {
    std::uint64_t startAddr{kBBAddrNone};
    std::uint64_t endAddr  {kBBAddrNone};
    std::uint64_t successor0{kBBAddrNone};
    std::uint64_t successor1{kBBAddrNone};
    std::uint32_t insnCount{0};
    std::uint32_t flags{0};

    bool endsWithRet() const noexcept { return (flags & BB_ENDS_RET) != 0; }
    bool endsWithJmp() const noexcept { return (flags & BB_ENDS_JMP) != 0; }
    bool endsWithJcc() const noexcept { return (flags & BB_ENDS_JCC) != 0; }
    bool hasCall()     const noexcept { return (flags & BB_HAS_CALL) != 0; }
    bool isInvalid()   const noexcept { return (flags & BB_INVALID)  != 0; }
    std::uint64_t sizeBytes() const noexcept { return endAddr - startAddr; }
};

class CUDADisassembler {
public:
    explicit CUDADisassembler(CUDAContext* ctx = nullptr);
    ~CUDADisassembler();

    CUDADisassembler(CUDADisassembler&&)            = default;
    CUDADisassembler& operator=(CUDADisassembler&&) = default;

    std::vector<BasicBlock> disassemble(const std::uint8_t*          codeBytes,
                                        std::size_t                   codeSize,
                                        std::uint64_t                 baseVMA,
                                        const std::vector<std::uint64_t>& entryVMAs);

    bool               usesGPU()    const noexcept { return ctx_ && gpuReady_; }
    const std::string& lastError()  const noexcept { return lastError_; }

private:
    std::vector<BasicBlock> disassembleCPU(const std::uint8_t*          bytes,
                                           std::size_t                   size,
                                           std::uint64_t                 base,
                                           const std::vector<std::uint64_t>& seeds);

    CUDAContext* ctx_{nullptr};
    bool         gpuReady_{false};
    std::string  lastError_;
};

} // namespace retdec::cuda_accel
