/**
 * @file include/retdec/pyc_parser/pyc_magic.h
 * @brief Python .pyc magic number table and version detection.
 *
 * ## .pyc File Layout
 *
 * ```
 * Offset  Size  Field
 * ──────  ────  ──────────────────────────────────────────────────────────
 *      0     4  Magic number (little-endian uint32)
 *               Lower 16 bits: version-specific "magic" word
 *               Upper 16 bits: always 0x0D0A (CRLF marker for text safety)
 *      4     4  Bit field:
 *               Bit 0: if set, use source_hash mode (Python 3.8+)
 *               Bit 1: if set, hash validation is checked (not unchecked)
 *      8     4  mtime (source last-modified, Unix timestamp)   if bit 0 == 0
 *      8     8  source_hash (SipHash-2-4 of source)            if bit 0 == 1
 *     12     4  source_size (bytes)                            if bit 0 == 0
 *     16     N  Marshalled code object (TYPE_CODE)
 * ```
 *
 * Note: In Python ≤ 3.7 the header was smaller (8 bytes).  We only
 * support 3.8+ which always uses the 16-byte header.
 *
 * ## Magic number history (CPython)
 *
 * Every CPython release that changes the bytecode format bumps the magic
 * number.  We record the *first* magic for each feature release; interim
 * alpha/beta bumps in the same feature release share the major version.
 *
 * Source: Lib/importlib/_bootstrap_external.py  MAGIC_NUMBER
 */

#ifndef RETDEC_PYC_PARSER_PYC_MAGIC_H
#define RETDEC_PYC_PARSER_PYC_MAGIC_H

#include <cstdint>
#include <optional>
#include <string>

namespace retdec {
namespace pyc_parser {

// ─── PythonVersion ────────────────────────────────────────────────────────────

/**
 * @brief Identifies a CPython bytecode format version.
 *
 * `major` and `minor` mirror CPython's sys.version_info.
 * `magic` is the 16-bit lower half of the .pyc magic uint32.
 */
struct PythonVersion {
    int      major  = 0;
    int      minor  = 0;
    uint16_t magic  = 0;   ///< Lower 16 bits of the .pyc magic field
    const char* name = ""; ///< Human-readable, e.g. "3.11"

    bool operator==(const PythonVersion& o) const noexcept {
        return major == o.major && minor == o.minor;
    }
    bool operator<(const PythonVersion& o) const noexcept {
        return major < o.major || (major == o.major && minor < o.minor);
    }
    bool atLeast(int maj, int min) const noexcept {
        return major > maj || (major == maj && minor >= min);
    }
};

// ─── Known versions ───────────────────────────────────────────────────────────

/**
 * @brief The full table of supported Python versions and their magic numbers.
 *
 * Listed in ascending order.  Multiple magic numbers can map to the same
 * feature version (alpha/beta bumps); we normalise to the latest stable.
 */
struct MagicEntry {
    uint16_t     magic;   ///< Lower 16 bits of .pyc magic
    PythonVersion ver;
};

/// Return the full table of known magic entries.
const MagicEntry* magicTable(size_t& count);

// ─── Detection API ────────────────────────────────────────────────────────────

/**
 * @brief Detect the Python version from a raw 4-byte magic number.
 *
 * The 4-byte magic is stored in little-endian order in the .pyc file.
 * Pass the raw 32-bit value directly from the file (already LE-decoded).
 *
 * @return The matching PythonVersion, or std::nullopt if unrecognised.
 */
std::optional<PythonVersion> detectVersion(uint32_t rawMagic);

/**
 * @brief Detect from a byte buffer (at least 4 bytes).
 */
std::optional<PythonVersion> detectVersion(const uint8_t* buf, size_t len);

/**
 * @brief Return the expected 32-bit magic for a given Python version.
 *
 * Returns 0 if the version is not in the table.
 */
uint32_t magicForVersion(int major, int minor);

/**
 * @brief Human-readable description of a .pyc header.
 */
std::string describeHeader(uint32_t magic, uint32_t bitField,
                            uint32_t mtimeOrHash, uint32_t sourceSize);

// ─── Constants ────────────────────────────────────────────────────────────────

/// All supported .pyc files have the CRLF marker in the upper 16 bits.
static constexpr uint16_t kPycCRLFMarker = 0x0D0A;

/// Minimum supported Python version (inclusive)
static constexpr int kMinMajor = 3;
static constexpr int kMinMinor = 8;

/// Maximum supported Python version (inclusive – extensible)
static constexpr int kMaxMajor = 3;
static constexpr int kMaxMinor = 13;

/// .pyc header size for Python 3.8+
static constexpr size_t kPycHeaderSize = 16;

/// Bit 0 of the bit_field: source-hash mode (vs mtime mode)
static constexpr uint32_t kBitFieldSourceHash = 1u << 0;

/// Bit 1 of the bit_field: hash validation is checked
static constexpr uint32_t kBitFieldChecked    = 1u << 1;

} // namespace pyc_parser
} // namespace retdec

#endif // RETDEC_PYC_PARSER_PYC_MAGIC_H
