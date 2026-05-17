/**
 * @file src/cli_parser/cli_heaps.cpp
 * @brief .NET CLI metadata heap readers.
 */

#include "retdec/cli_parser/cli_heaps.h"

#include <cassert>
#include <cstring>

namespace retdec {
namespace cli_parser {

// ─── Compressed integer decoding ─────────────────────────────────────────────

std::optional<uint32_t> decodeCompressedUInt(
        std::span<const uint8_t> blob, size_t& pos) {
    if (pos >= blob.size()) return std::nullopt;

    uint8_t b0 = blob[pos];
    if ((b0 & 0x80) == 0) {
        // 1-byte encoding: 0xxxxxxx
        ++pos;
        return static_cast<uint32_t>(b0);
    }
    if ((b0 & 0xC0) == 0x80) {
        // 2-byte encoding: 10xxxxxx xxxxxxxx
        if (pos + 1 >= blob.size()) return std::nullopt;
        uint8_t b1 = blob[pos + 1];
        pos += 2;
        return (static_cast<uint32_t>(b0 & 0x3F) << 8) | b1;
    }
    if ((b0 & 0xE0) == 0xC0) {
        // 4-byte encoding: 110xxxxx xxxxxxxx xxxxxxxx xxxxxxxx
        if (pos + 3 >= blob.size()) return std::nullopt;
        uint8_t b1 = blob[pos + 1];
        uint8_t b2 = blob[pos + 2];
        uint8_t b3 = blob[pos + 3];
        pos += 4;
        return (static_cast<uint32_t>(b0 & 0x1F) << 24) |
               (static_cast<uint32_t>(b1)         << 16) |
               (static_cast<uint32_t>(b2)         <<  8) |
                static_cast<uint32_t>(b3);
    }
    return std::nullopt;
}

std::optional<int32_t> decodeCompressedInt(
        std::span<const uint8_t> blob, size_t& pos) {
    auto uval = decodeCompressedUInt(blob, pos);
    if (!uval) return std::nullopt;

    // Sign-extend: the value is stored in a "rotate" encoding.
    // The LSB of the decoded uint is the sign bit; shift right by 1 then sign-extend.
    uint32_t u = *uval;
    bool negative = (u & 1) != 0;
    int32_t val = static_cast<int32_t>(u >> 1);
    if (negative) {
        // Sign-extend based on original bit width
        if (*uval <= 0x3F)        val |= static_cast<int32_t>(0xFFFFFFC0u);
        else if (*uval <= 0x3FFF) val |= static_cast<int32_t>(0xFFFFE000u);
        else                      val |= static_cast<int32_t>(0xF0000000u);
    }
    return val;
}

// ─── StringsHeap ─────────────────────────────────────────────────────────────

StringsHeap::StringsHeap(std::span<const uint8_t> data) : data_(data) {}

std::string StringsHeap::get(uint32_t offset) const {
    if (offset >= data_.size()) return "";
    const char* ptr = reinterpret_cast<const char*>(data_.data() + offset);
    size_t maxLen = data_.size() - offset;
    size_t len = strnlen(ptr, maxLen);
    return std::string(ptr, len);
}

// ─── UserStringsHeap ─────────────────────────────────────────────────────────

UserStringsHeap::UserStringsHeap(std::span<const uint8_t> data) : data_(data) {}

std::string UserStringsHeap::utf16leToUtf8(const uint8_t* src, size_t chars) {
    std::string out;
    out.reserve(chars);
    for (size_t i = 0; i < chars; ++i) {
        uint16_t cp = static_cast<uint16_t>(src[i * 2]) |
                      (static_cast<uint16_t>(src[i * 2 + 1]) << 8);
        if (cp < 0x80) {
            out.push_back(static_cast<char>(cp));
        } else if (cp < 0x800) {
            out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        } else {
            out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        }
    }
    return out;
}

std::string UserStringsHeap::get(uint32_t offset) const {
    if (offset >= data_.size()) return "";
    size_t pos = offset;
    auto sub = data_.subspan(pos);
    auto lenOpt = decodeCompressedUInt(sub, pos);
    if (!lenOpt || *lenOpt == 0) return "";

    uint32_t totalBytes = *lenOpt;
    // The blob contains UTF-16LE chars + 1 trailing byte
    uint32_t byteCount = totalBytes > 0 ? totalBytes - 1 : 0;
    uint32_t charCount = byteCount / 2;

    size_t dataStart = offset + pos;
    if (dataStart + byteCount > data_.size()) return "";

    return utf16leToUtf8(data_.data() + dataStart, charCount);
}

// ─── GuidHeap ────────────────────────────────────────────────────────────────

GuidHeap::GuidHeap(std::span<const uint8_t> data) : data_(data) {}

Guid GuidHeap::get(uint32_t index) const {
    Guid g;
    if (index == 0) return g;
    size_t off = (index - 1) * 16;
    if (off + 16 > data_.size()) return g;
    std::memcpy(g.bytes, data_.data() + off, 16);
    return g;
}

std::string Guid::toString() const {
    char buf[37];
    snprintf(buf, sizeof(buf),
        "%02X%02X%02X%02X-%02X%02X-%02X%02X-%02X%02X-%02X%02X%02X%02X%02X%02X",
        bytes[3],  bytes[2],  bytes[1],  bytes[0],
        bytes[5],  bytes[4],
        bytes[7],  bytes[6],
        bytes[8],  bytes[9],
        bytes[10], bytes[11], bytes[12], bytes[13], bytes[14], bytes[15]);
    return buf;
}

bool Guid::operator==(const Guid& o) const {
    return std::memcmp(bytes, o.bytes, 16) == 0;
}

// ─── BlobHeap ─────────────────────────────────────────────────────────────────

BlobHeap::BlobHeap(std::span<const uint8_t> data) : data_(data) {}

std::span<const uint8_t> BlobHeap::get(uint32_t offset) const {
    if (offset >= data_.size()) return {};
    size_t pos = 0;
    auto sub = data_.subspan(offset);
    auto lenOpt = decodeCompressedUInt(sub, pos);
    if (!lenOpt) return {};
    uint32_t len = *lenOpt;
    size_t dataStart = offset + pos;
    if (dataStart + len > data_.size()) return {};
    return data_.subspan(dataStart, len);
}

std::vector<uint8_t> BlobHeap::getVec(uint32_t offset) const {
    auto s = get(offset);
    return std::vector<uint8_t>(s.begin(), s.end());
}

// ─── CliHeaps ─────────────────────────────────────────────────────────────────

CliHeaps::CliHeaps(std::span<const uint8_t> strData,
                    std::span<const uint8_t> usData,
                    std::span<const uint8_t> guidData,
                    std::span<const uint8_t> blobData,
                    uint8_t                  hs)
    : strings(strData)
    , userStrings(usData)
    , guids(guidData)
    , blobs(blobData)
    , heapSizes(hs)
{}

} // namespace cli_parser
} // namespace retdec
