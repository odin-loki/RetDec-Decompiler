/**
 * @file include/retdec/opencl/ocl_disassembler.h
 * @brief OpenCL-accelerated parallel CFG disassembler.
 *
 * OCLDisassembler dispatches multiple CFG seeds in parallel across OpenCL
 * work-items.  An atomic visited-bitset prevents duplicate work.  The host
 * merges per-work-item basic blocks, resolves cross-seed edges, and returns a
 * canonical sorted CFG.
 *
 * Fallback: when OpenCL is unavailable, a simple sequential x86-64 walker is
 * used via the same public API.
 */

#ifndef RETDEC_OPENCL_OCL_DISASSEMBLER_H
#define RETDEC_OPENCL_OCL_DISASSEMBLER_H

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace retdec {
namespace opencl {

class OCLContext;

// ─── Basic block descriptor (mirrors the OpenCL struct) ──────────────────────

static constexpr std::uint64_t kBBAddrNone = 0xFFFFFFFFFFFFFFFFULL;

/// Flags matching the OpenCL kernel's RETDEC_BB_FLAG_* values.
enum BasicBlockFlags : std::uint32_t {
    BB_HAS_CALL  = 1u,
    BB_ENDS_RET  = 2u,
    BB_ENDS_JMP  = 4u,
    BB_ENDS_JCC  = 8u,
    BB_INVALID   = 16u,
};

struct BasicBlock {
    std::uint64_t startAddr  = kBBAddrNone;
    std::uint64_t endAddr    = kBBAddrNone;  ///< exclusive
    std::uint64_t successor0 = kBBAddrNone;  ///< fall-through / JMP target
    std::uint64_t successor1 = kBBAddrNone;  ///< JCC taken-branch target
    std::uint32_t insnCount  = 0;
    std::uint32_t flags      = 0;

    bool endsWithRet()  const noexcept { return (flags & BB_ENDS_RET) != 0; }
    bool endsWithJmp()  const noexcept { return (flags & BB_ENDS_JMP) != 0; }
    bool endsWithJcc()  const noexcept { return (flags & BB_ENDS_JCC) != 0; }
    bool hasCall()      const noexcept { return (flags & BB_HAS_CALL) != 0; }
    bool isInvalid()    const noexcept { return (flags & BB_INVALID)  != 0; }
    std::size_t sizeBytes() const noexcept {
        return (endAddr != kBBAddrNone && startAddr != kBBAddrNone && endAddr > startAddr)
               ? static_cast<std::size_t>(endAddr - startAddr) : 0;
    }
};

// ─── OCLDisassembler ──────────────────────────────────────────────────────────

class OCLDisassembler {
public:
    /// @param ctx  Initialized OCLContext.  If null or not ready, falls back
    ///             to CPU sequential mode.
    explicit OCLDisassembler(OCLContext* ctx = nullptr);
    ~OCLDisassembler();

    OCLDisassembler(OCLDisassembler&&) noexcept;
    OCLDisassembler& operator=(OCLDisassembler&&) noexcept;

    OCLDisassembler(const OCLDisassembler&)            = delete;
    OCLDisassembler& operator=(const OCLDisassembler&) = delete;

    /// Disassemble a code section.
    ///
    /// @param codeBytes   Raw bytes of the code section.
    /// @param codeSize    Number of valid bytes.
    /// @param baseVMA     Virtual address corresponding to codeBytes[0].
    /// @param entryVMAs   List of entry-point VMAs to start disassembly from.
    ///                    Must be within [baseVMA, baseVMA+codeSize).
    /// @returns Sorted, de-duplicated list of basic blocks.
    std::vector<BasicBlock>
    disassemble(const std::uint8_t*               codeBytes,
                std::size_t                        codeSize,
                std::uint64_t                      baseVMA,
                const std::vector<std::uint64_t>&  entryVMAs);

    /// Whether the OpenCL path is active.
    bool usesGPU() const noexcept;

    /// Last error description (empty if none).
    const std::string& lastError() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> _impl;
};

} // namespace opencl
} // namespace retdec

#endif // RETDEC_OPENCL_OCL_DISASSEMBLER_H
