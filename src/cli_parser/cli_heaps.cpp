/**
 * @file src/cli_parser/cli_heaps.cpp
 * @brief .NET CLI metadata heap readers.
 */

#include "retdec/cli_parser/cli_heaps.h"

#include "retdec/utils/bounds.h"
#include "retdec/utils/compressed_int.h"
#include "retdec/utils/index_translation.h"
#include "retdec/utils/text_transcode.h"

#include <cassert>
#include <cstring>

namespace retdec {
namespace cli_parser {

// ─── Compressed integer decoding ─────────────────────────────────────────────

// Both decoders are now one call into utils/compressed_int.h, which owns
// ECMA-335 II.23.2 for the whole tree and is proved over the entire 64-bit
// domain in tests/verification/compressed_int_proof.cpp. The two things that
// were wrong here are stated once, where the fix is, rather than being
// re-derived at each of the sixteen call sites that read a compressed integer.

std::optional<uint32_t> decodeCompressedUInt(
        std::span<const uint8_t> blob, size_t& pos) {
    // The width guards used to read `if (pos + 1 >= blob.size())` and
    // `if (pos + 3 >= blob.size())`, forming a sum on a cursor the file
    // controls. ESBMC: pos = 0xFFFFFFFFFFFFFFFD, size = 1 -- pos + 3 wraps to
    // 0, `0 >= 1` is false, the guard admits the read, and the four fetches
    // that follow are past the end of a one-byte blob. cint::decodeUnsigned
    // states the same bound as bounds::rangeFits, which compares against the
    // bytes remaining and never forms pos + width.
    const utils::cint::Result r =
        utils::cint::decodeUnsigned(blob.data(), blob.size(), pos);
    if (!r.ok) return std::nullopt;
    // A refusal consumes nothing, so the cursor only ever moves on success --
    // the leb128.h convention, which is what makes a failed decode retryable
    // instead of desynchronising the rest of the blob.
    pos += r.bytesRead;
    return r.value;
}

std::optional<int32_t> decodeCompressedInt(
        std::span<const uint8_t> blob, size_t& pos) {
    // The sign-extension mask used to be picked from the decoded MAGNITUDE:
    //
    //     if      (*uval <= 0x3F)   val |= 0xFFFFFFC0u;
    //     else if (*uval <= 0x3FFF) val |= 0xFFFFE000u;
    //     else                      val |= 0xF0000000u;
    //
    // 0x3F and 0x3FFF are the VALUE ranges of the 1- and 2-byte forms, not
    // their tag ranges: a one-byte encoding spans 0x00..0x7F. ECMA-335 II.23.2
    // gives 0x7F as the encoding of -1; the magnitude 63 took the two-byte
    // branch and the function returned 63 | 0xFFFFE000 = -8129. Every odd byte
    // in 0x41..0x7F was wrong the same way (0x41 is -32, returned as -8160).
    // Nor was the one-byte form the only casualty -- the same magnitude is a
    // legal payload at all three widths, so no test on the magnitude can be
    // right for more than one of them: 0x80 0x3F means -8161 and came back as
    // -33, and 0xC0 0x00 0x24 0x7F means -268430785 and came back as -3521.
    // These are array lower bounds (cli_sig.cpp decodeArrayShape), so an array
    // declared [-1..n] decompiled as [-8129..n].
    //
    // cint::decodeSigned takes the payload width from payloadBits(bytesRead)
    // and from nothing else.
    const utils::cint::SResult r =
        utils::cint::decodeSigned(blob.data(), blob.size(), pos);
    if (!r.ok) return std::nullopt;
    pos += r.bytesRead;
    return r.value;
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

std::string UserStringsHeap::utf16leToUtf8(
        const uint8_t* src, size_t srcBytes, size_t chars) {
    // The loop this replaces took the three-byte `else` branch for every code
    // unit at or above 0x800, surrogates included. A surrogate is not a scalar
    // value and has no UTF-8 encoding: ESBMC's witness cp = 0xD800 came out as
    // ED A0 80, which no UTF-8 decoder accepts, and a surrogate PAIR -- the
    // entire reason UTF-16 has surrogates -- came out as two such ill-formed
    // sequences instead of the one supplementary character it denotes.
    //
    // txt::utf16leToUtf8 pairs surrogates, replaces a lone one with U+FFFD, and
    // is proved never to emit an ED A0..BF lead byte
    // (proof_encode_utf8_is_well_formed, proof_utf16_never_emits_a_surrogate).
    // It is also proved byte-identical to this loop on every input containing
    // no surrogate unit (proof_the_kernel_matches_dotnet_off_the_surrogates),
    // so routing here changes the output only where it was already ill-formed.
    //
    // The bound on `src` is a parameter now rather than a promise the caller
    // makes. It used to be a unit count alone, which meant the only thing
    // keeping the read inside the heap was a rangeFits in the caller -- correct
    // today, and nothing a second caller would know to repeat. The kernel wants
    // both numbers, and both are passed straight through.
    //
    // The two are checked against each other rather than one derived from the
    // other: `chars` is what the length prefix declared and `srcBytes` is what
    // is really there, and they are two separate statements about the same
    // range. txt::utf8CapacityForUtf16 then checks the 3-bytes-per-unit product
    // with bounds::mulFits, and the transcode refuses to start unless the
    // capacity covers it.
    if (!utils::bounds::mulFits(chars, 2)) return "";
    const size_t inBytes = chars * 2;
    if (inBytes > srcBytes) return "";
    const size_t cap = utils::txt::utf8CapacityForUtf16(chars);
    if (inBytes == 0 || cap == 0) return "";

    std::string out(cap, '\0');
    const size_t written =
        utils::txt::utf16leToUtf8(src, inBytes, out.data(), out.size());
    out.resize(written);
    return out;
}

std::string UserStringsHeap::get(uint32_t offset) const {
    // The cursor used to be initialised to `offset` and then handed to a span
    // that had ALREADY been advanced by `offset`, so the length prefix was read
    // from data_[2 * offset] and the characters from 2 * offset + bytesRead.
    // ESBMC: at offset = 2 the byte examined is data_[4] while the prefix sits
    // at data_[2]; at offset = 8 with a 1-byte prefix, dataStart came out 17
    // where the UTF-16 units actually begin at 9. Every user string at a
    // non-zero heap offset decoded with a length taken from the wrong byte.
    // BlobHeap::get below is the same read written correctly, which is what
    // made this a slip rather than a convention.
    //
    // Decoding straight out of the whole heap at absolute `offset` removes the
    // second origin entirely: there is now one position, not a span-relative
    // one and a heap-relative one that had drifted apart.
    const utils::cint::Result len =
        utils::cint::decodeUnsigned(data_.data(), data_.size(), offset);
    if (!len.ok || len.value == 0) return "";

    // ECMA-335 II.24.2.4: the blob is the UTF-16LE units followed by one
    // trailing flag byte, so the character bytes are one fewer than the
    // declared length. len.value is non-zero here, so this cannot wrap.
    const uint32_t byteCount = len.value - 1;
    const uint32_t charCount = byteCount / 2;

    // decodeUnsigned succeeded, so rangeFits(offset, size, bytesRead) held and
    // this sum is inside the heap -- it cannot wrap.
    const size_t dataStart = offset + len.bytesRead;
    // Was `dataStart + byteCount > data_.size()`, an addition on a
    // file-declared length. rangeFits compares against the bytes remaining.
    if (!utils::bounds::rangeFits(dataStart, data_.size(), byteCount)) return "";

    // byteCount is the bytes rangeFits just cleared, and charCount is
    // byteCount / 2, so the helper gets the real extent and the declared count
    // separately and can compare them itself.
    return utf16leToUtf8(data_.data() + dataStart, byteCount, charCount);
}

// ─── GuidHeap ────────────────────────────────────────────────────────────────

GuidHeap::GuidHeap(std::span<const uint8_t> data) : data_(data) {}

/// Bytes one #GUID heap entry occupies (ECMA-335 II.24.2.5: the heap is a
/// sequence of 16-byte GUIDs addressed by a 1-based index).
static constexpr size_t kGuidBytes = sizeof(Guid::bytes);

Guid GuidHeap::get(uint32_t index) const {
    Guid g;
    // Was `size_t off = (index - 1) * 16;`. `index` is uint32_t and `16` is an
    // int, so the product was formed in 32 bits and wrapped before it ever
    // reached the size_t. ESBMC: index = 0x10000001 on a heap holding exactly
    // one GUID -- the ordinary case, an assembly has one MVID -- gives a true
    // offset of 4294967296, which wraps to 0; `0 + 16 > 16` is false, so the
    // bounds test passed and the function memcpy'd the module's own MVID and
    // reported success for an index that addresses nothing. index = 805306623
    // on a 4080-byte heap is the same fault at a less round witness: true
    // offset 12884905952, wrapped offset 4064, the LAST GUID returned. No
    // caller can tell, because there is no failure signal at all.
    //
    // idxmap::rowAt1Based does the whole translation at 64 bits: it refuses
    // index 0 (the null token) rather than wrapping to SIZE_MAX, checks the
    // multiplication with bounds::mulFits, and only then forms the sum.
    size_t off = 0;
    if (!utils::idxmap::rowAt1Based(data_.size(), index, kGuidBytes, off))
        return g;
    std::memcpy(g.bytes, data_.data() + off, kGuidBytes);
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
