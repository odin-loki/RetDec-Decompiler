/**
 * @file include/retdec/mini_emu/mini_unpacker.h
 * @brief High-level unpacker façade: runs MiniEmu, dumps memory, rebuilds section table.
 */

#pragma once

#include "retdec/mini_emu/mini_emu.h"
#include "retdec/fileformat/lattice/format_result.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace retdec {
namespace mini_emu {

/**
 * High-level unpacker result.
 *
 * On success the caller can re-run the FormatLattice parser on
 * `dump` to get the fully parsed unpacked binary.
 */
struct UnpackerOutput {
    bool     success       = false;
    std::string errorMsg;

    /// Raw bytes of the reconstructed unpacked image.
    std::vector<uint8_t> dump;

    /// Synthetic section table for the dump.
    std::vector<SectionInfo> sections;

    /// Detected entry point in the dump (file-offset based).
    uint64_t entryPointOffset = 0;

    /// Reason emulation stopped.
    StopReason stopReason = StopReason::Error;

    uint64_t instructionsExecuted = 0;
    bool     needsManualReview    = false;
};

// ─── MiniUnpacker ─────────────────────────────────────────────────────────────

class MiniUnpacker {
public:
    MiniUnpacker() = default;

    /**
     * Unpack a packed binary image.
     *
     * @param data          Raw packed binary bytes.
     * @param size          File size.
     * @param fmt           FormatResult from Stage 1 parser (entry point, sections).
     * @param maxInsns      Instruction limit for the emulator.
     */
    UnpackerOutput unpack(const uint8_t *data, size_t size,
                          const FormatResult &fmt,
                          uint64_t maxInsns = kMaxInstructions) const;

private:
    /// Reconstruct a dump from written+executed pages.
    std::vector<uint8_t> buildDump(const MiniEmu &emu,
                                   const std::vector<UnpackedRegion> &regions,
                                   uint64_t &outBase,
                                   std::vector<SectionInfo> &outSections) const;

    /// Scan a region for common function prologues and flag as code.
    static bool looksLikeCode(const uint8_t *data, size_t size);
};

} // namespace mini_emu
} // namespace retdec
