/**
 * @file include/retdec/fileformat/lattice/format_lattice.h
 * @brief Signature-Lattice file format parser (Stage 1).
 *
 * Replaces sequential format trial with a decision tree over the first
 * 512 bytes of the input.  Every parse path terminates in a specific
 * parser leaf (ELF32, ELF64, PE32, PE64, MachO32/64, COFF, IntelHex,
 * Raw).  AR archives dispatch each member in parallel via std::async.
 *
 * Malformed-field recovery: when a plausibility check fails (entry point
 * outside load range, insane section count, etc.) the field is flagged in
 * FormatResult::corruption and a heuristic fallback is used.
 *
 * Polyglot scoring: for files whose magic is ambiguous (e.g. 0xCAFEBABE
 * which is both Java .class and fat Mach-O) we count self-consistent
 * internal cross-references per candidate and pick the highest scorer.
 */

#pragma once

#include "retdec/fileformat/lattice/format_result.h"

#include <cstddef>
#include <cstdint>
#include <string>

namespace retdec {
namespace fileformat {
namespace lattice {

/**
 * Signature-Lattice parser.
 *
 * All classify() overloads:
 *  1. Read up to the first 512 bytes.
 *  2. Dispatch via the magic-byte decision tree.
 *  3. Parse the full header (section table, imports, exports, TLS).
 *  4. Run plausibility checks and apply malformed-field recovery.
 *  5. Return a populated FormatResult.
 */
class FormatLattice {
public:
    FormatLattice()  = default;
    ~FormatLattice() = default;

    FormatLattice(const FormatLattice &) = delete;
    FormatLattice &operator=(const FormatLattice &) = delete;

    /**
     * Classify a binary image already in memory.
     *
     * @param data     Pointer to file bytes (or member bytes for AR).
     * @param size     Total number of bytes available.
     * @param name     Optional human-readable name (file path, member name).
     * @return         Populated FormatResult.
     */
    FormatResult classify(const uint8_t *data, size_t size,
                          const std::string&name = std::string{}) const;

    /**
     * Classify a file on disk.  Reads the entire file into memory.
     */
    FormatResult classifyFile(const std::string &path) const;
};

} // namespace lattice
} // namespace fileformat
} // namespace retdec
