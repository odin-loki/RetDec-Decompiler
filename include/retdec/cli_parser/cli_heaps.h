/**
 * @file include/retdec/cli_parser/cli_heaps.h
 * @brief .NET CLI metadata heap readers.
 *
 * ## Heaps (ECMA-335 §II.24.2.3 – §II.24.2.6)
 *
 * ### #Strings heap
 *   UTF-8 null-terminated strings.  Index 0 is always the empty string.
 *   Accessed via a 16-bit or 32-bit offset (determined by HeapSizes bit 0).
 *
 * ### #US (User Strings) heap
 *   Blob-encoded UTF-16LE strings with a trailing byte indicating whether
 *   the string contains any non-ASCII characters.
 *   Accessed via a 16-bit or 32-bit offset (HeapSizes bit 2).
 *   Index encoded in ldstr token as low 24 bits of 0x70XXXXXX.
 *
 * ### #GUID heap
 *   16-byte GUIDs.  Accessed via a 1-based 16-bit or 32-bit index.
 *   (HeapSizes bit 1).
 *
 * ### #Blob heap
 *   Variable-length blobs with a compressed-uint length prefix.
 *   Used for: method signatures, field signatures, type specs, custom
 *   attribute blobs, constant values, public key blobs.
 *   Accessed via a 16-bit or 32-bit offset (HeapSizes bit 2).
 *
 * ## Compressed integers (§II.23.2)
 *
 * Blob lengths and some signature values use a compressed unsigned int:
 *   - If byte[0] & 0x80 == 0: value = byte[0] (1 byte)
 *   - If byte[0] & 0xC0 == 0x80: value = (byte[0] & 0x3F) << 8 | byte[1] (2 bytes)
 *   - If byte[0] & 0xE0 == 0xC0: 4 bytes, bits [28:0] from 3 low bytes of first word
 *
 * Compressed signed ints (§II.23.2.6) are additionally sign-extended from bit 0.
 */

#ifndef RETDEC_CLI_PARSER_CLI_HEAPS_H
#define RETDEC_CLI_PARSER_CLI_HEAPS_H

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace retdec {
namespace cli_parser {

// ─── HeapSizes flags (from #~ stream header) ─────────────────────────────────

static constexpr uint8_t kHeapSizeStrings = 0x01; ///< #Strings indices are 4 bytes
static constexpr uint8_t kHeapSizeGUID    = 0x02; ///< #GUID indices are 4 bytes
static constexpr uint8_t kHeapSizeBlob    = 0x04; ///< #Blob indices are 4 bytes

// ─── Compressed integer decoding ─────────────────────────────────────────────

/**
 * @brief Decode a compressed unsigned integer from a byte span.
 *
 * Advances `pos` past the decoded bytes.
 * Returns std::nullopt if the span is too short or the encoding is invalid.
 */
std::optional<uint32_t> decodeCompressedUInt(
    std::span<const uint8_t> blob, size_t& pos);

/**
 * @brief Decode a compressed signed integer from a byte span.
 *
 * Like decodeCompressedUInt but sign-extends the result.
 */
std::optional<int32_t> decodeCompressedInt(
    std::span<const uint8_t> blob, size_t& pos);

// ─── #Strings heap ───────────────────────────────────────────────────────────

class StringsHeap {
public:
    explicit StringsHeap(std::span<const uint8_t> data);

    /// Read a null-terminated UTF-8 string at `offset`.
    std::string get(uint32_t offset) const;

    bool empty() const { return data_.empty(); }
    size_t size() const { return data_.size(); }

private:
    std::span<const uint8_t> data_;
};

// ─── #US heap ────────────────────────────────────────────────────────────────

class UserStringsHeap {
public:
    explicit UserStringsHeap(std::span<const uint8_t> data);

    /// Read a UTF-16LE user string at `offset`.  Returns UTF-8.
    std::string get(uint32_t offset) const;

    bool empty() const { return data_.empty(); }

private:
    std::span<const uint8_t> data_;

    static std::string utf16leToUtf8(const uint8_t* src, size_t chars);
};

// ─── #GUID heap ───────────────────────────────────────────────────────────────

struct Guid {
    uint8_t bytes[16] = {};

    std::string toString() const;
    bool operator==(const Guid& o) const;
};

class GuidHeap {
public:
    explicit GuidHeap(std::span<const uint8_t> data);

    /// Read a GUID by 1-based index.  Returns all-zeros GUID for index 0.
    Guid get(uint32_t index) const;

    bool empty() const { return data_.empty(); }

private:
    std::span<const uint8_t> data_;
};

// ─── #Blob heap ───────────────────────────────────────────────────────────────

class BlobHeap {
public:
    explicit BlobHeap(std::span<const uint8_t> data);

    /// Read a blob at `offset`.  Returns the raw bytes (no length prefix).
    std::span<const uint8_t> get(uint32_t offset) const;

    /// Convenience: read blob and return as vector.
    std::vector<uint8_t> getVec(uint32_t offset) const;

    bool empty() const { return data_.empty(); }
    size_t size() const { return data_.size(); }

private:
    std::span<const uint8_t> data_;
};

// ─── Combined heap set ────────────────────────────────────────────────────────

struct CliHeaps {
    StringsHeap     strings;
    UserStringsHeap userStrings;
    GuidHeap        guids;
    BlobHeap        blobs;
    uint8_t         heapSizes = 0; ///< From #~ header

    CliHeaps(std::span<const uint8_t> strData,
             std::span<const uint8_t> usData,
             std::span<const uint8_t> guidData,
             std::span<const uint8_t> blobData,
             uint8_t                  heapSizes);

    /// Returns true if #Strings indices are 4-byte wide.
    bool wideStrings() const { return (heapSizes & kHeapSizeStrings) != 0; }
    /// Returns true if #GUID indices are 4-byte wide.
    bool wideGuid()    const { return (heapSizes & kHeapSizeGUID)    != 0; }
    /// Returns true if #Blob indices are 4-byte wide.
    bool wideBlob()    const { return (heapSizes & kHeapSizeBlob)    != 0; }
};

} // namespace cli_parser
} // namespace retdec

#endif // RETDEC_CLI_PARSER_CLI_HEAPS_H
