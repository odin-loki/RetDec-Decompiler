/**
 * @file src/pyc_parser/pyc_magic.cpp
 * @brief Python .pyc magic number table and version detection.
 */

#include "retdec/pyc_parser/pyc_magic.h"

#include <cstring>
#include <sstream>

namespace retdec {
namespace pyc_parser {

// ─── Magic number table ───────────────────────────────────────────────────────
// Source: CPython Lib/importlib/_bootstrap_external.py MAGIC_NUMBER
// and Tools/scripts/generate_opcode_h.py for per-release bumps.
//
// Only the first stable magic for each x.y release is listed here.
// Alpha/beta bumps within the same release map to the same PythonVersion.

static const MagicEntry kMagicTable[] = {
    // Python 3.8
    {3413, {3, 8, 3413, "3.8"}},
    {3414, {3, 8, 3414, "3.8"}},
    {3415, {3, 8, 3415, "3.8"}},
    {3416, {3, 8, 3416, "3.8"}},
    {3417, {3, 8, 3417, "3.8"}},
    {3418, {3, 8, 3418, "3.8"}},
    {3419, {3, 8, 3419, "3.8"}},
    // Python 3.9
    {3425, {3, 9, 3425, "3.9"}},
    {3426, {3, 9, 3426, "3.9"}},
    {3427, {3, 9, 3427, "3.9"}},
    {3428, {3, 9, 3428, "3.9"}},
    {3429, {3, 9, 3429, "3.9"}},
    {3430, {3, 9, 3430, "3.9"}},
    {3431, {3, 9, 3431, "3.9"}},
    {3432, {3, 9, 3432, "3.9"}},
    {3433, {3, 9, 3433, "3.9"}},
    {3434, {3, 9, 3434, "3.9"}},
    // Python 3.10
    {3439, {3, 10, 3439, "3.10"}},
    {3440, {3, 10, 3440, "3.10"}},
    {3441, {3, 10, 3441, "3.10"}},
    {3442, {3, 10, 3442, "3.10"}},
    {3443, {3, 10, 3443, "3.10"}},
    {3444, {3, 10, 3444, "3.10"}},
    {3445, {3, 10, 3445, "3.10"}},
    {3446, {3, 10, 3446, "3.10"}},
    {3447, {3, 10, 3447, "3.10"}},
    {3448, {3, 10, 3448, "3.10"}},
    {3449, {3, 10, 3449, "3.10"}},
    {3450, {3, 10, 3450, "3.10"}},
    // Python 3.11
    {3495, {3, 11, 3495, "3.11"}},
    {3496, {3, 11, 3496, "3.11"}},
    {3497, {3, 11, 3497, "3.11"}},
    {3498, {3, 11, 3498, "3.11"}},
    {3499, {3, 11, 3499, "3.11"}},
    {3500, {3, 11, 3500, "3.11"}},
    {3501, {3, 11, 3501, "3.11"}},
    {3502, {3, 11, 3502, "3.11"}},
    {3503, {3, 11, 3503, "3.11"}},
    {3504, {3, 11, 3504, "3.11"}},
    {3505, {3, 11, 3505, "3.11"}},
    {3506, {3, 11, 3506, "3.11"}},
    {3507, {3, 11, 3507, "3.11"}},
    {3508, {3, 11, 3508, "3.11"}},
    {3509, {3, 11, 3509, "3.11"}},
    {3510, {3, 11, 3510, "3.11"}},
    {3511, {3, 11, 3511, "3.11"}},
    // Python 3.12
    {3531, {3, 12, 3531, "3.12"}},
    {3532, {3, 12, 3532, "3.12"}},
    {3533, {3, 12, 3533, "3.12"}},
    {3534, {3, 12, 3534, "3.12"}},
    {3535, {3, 12, 3535, "3.12"}},
    {3536, {3, 12, 3536, "3.12"}},
    {3537, {3, 12, 3537, "3.12"}},
    {3538, {3, 12, 3538, "3.12"}},
    {3539, {3, 12, 3539, "3.12"}},
    {3540, {3, 12, 3540, "3.12"}},
    {3541, {3, 12, 3541, "3.12"}},
    // Python 3.13 (tentative)
    {3561, {3, 13, 3561, "3.13"}},
    {3562, {3, 13, 3562, "3.13"}},
    {3563, {3, 13, 3563, "3.13"}},
    {3564, {3, 13, 3564, "3.13"}},
};

static constexpr size_t kMagicTableSize = sizeof(kMagicTable) / sizeof(kMagicTable[0]);

// ─── API ─────────────────────────────────────────────────────────────────────

const MagicEntry* magicTable(size_t& count) {
    count = kMagicTableSize;
    return kMagicTable;
}

std::optional<PythonVersion> detectVersion(uint32_t rawMagic) {
    // The lower 16 bits encode the version-specific magic word.
    // The upper 16 bits should be 0x0D0A (CRLF) but we don't enforce that
    // to be tolerant of slightly malformed files.
    uint16_t lo = static_cast<uint16_t>(rawMagic & 0xFFFF);
    uint16_t hi = static_cast<uint16_t>((rawMagic >> 16) & 0xFFFF);
    (void)hi; // tolerate non-standard upper bytes

    for (size_t i = 0; i < kMagicTableSize; ++i) {
        if (kMagicTable[i].magic == lo) {
            return kMagicTable[i].ver;
        }
    }
    return std::nullopt;
}

std::optional<PythonVersion> detectVersion(const uint8_t* buf, size_t len) {
    if (!buf || len < 4) return std::nullopt;
    uint32_t raw = static_cast<uint32_t>(buf[0])
                 | (static_cast<uint32_t>(buf[1]) << 8)
                 | (static_cast<uint32_t>(buf[2]) << 16)
                 | (static_cast<uint32_t>(buf[3]) << 24);
    return detectVersion(raw);
}

uint32_t magicForVersion(int major, int minor) {
    // Return the most commonly used (latest stable) magic for this version
    uint32_t best = 0;
    for (size_t i = 0; i < kMagicTableSize; ++i) {
        if (kMagicTable[i].ver.major == major && kMagicTable[i].ver.minor == minor) {
            // The table is in ascending order; keep the last (= latest)
            best = (static_cast<uint32_t>(kPycCRLFMarker) << 16)
                 | kMagicTable[i].magic;
        }
    }
    return best;
}

std::string describeHeader(uint32_t magic, uint32_t bitField,
                            uint32_t mtimeOrHash, uint32_t sourceSize) {
    std::ostringstream ss;
    auto ver = detectVersion(magic);
    if (ver) {
        ss << "Python " << ver->name;
    } else {
        ss << "Unknown Python (magic=0x" << std::hex << magic << std::dec << ")";
    }
    ss << ", ";
    if (bitField & kBitFieldSourceHash)
        ss << "source-hash mode";
    else
        ss << "mtime=" << mtimeOrHash << " size=" << sourceSize;
    return ss.str();
}

} // namespace pyc_parser
} // namespace retdec
