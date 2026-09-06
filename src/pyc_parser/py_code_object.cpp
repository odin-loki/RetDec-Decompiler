/**
 * @file src/pyc_parser/py_code_object.cpp
 * @brief PyCodeObject helpers and line-number table decoders.
 */

#include "retdec/pyc_parser/py_code_object.h"

#include <climits>

#include <algorithm>

namespace retdec {
namespace pyc_parser {

/// Add a line delta to a line number without overflowing.
///
/// `line` is an int32_t and the deltas come straight out of the line table, so
/// a long enough table walks it past INT32_MAX -- signed overflow, which is
/// undefined behaviour, not merely a wrong line number. It takes roughly a
/// 34 MB co_linetable to get there, which is why fuzzing never reached it.
///
/// Saturating is both safe and the right answer here: line numbers are bounded
/// by any real source file, a table claiming more lines than an int32 can count
/// is malformed, and clamping leaves the rest of the decode usable instead of
/// discarding it. The low end is clamped at 0 because a signed delta in the
/// 3.11 format can otherwise drive the line negative.
static int32_t addLineDelta(int32_t line, int64_t delta) noexcept {
    const int64_t sum = static_cast<int64_t>(line) + delta;
    if (sum > INT32_MAX) return INT32_MAX;
    if (sum < 0) return 0;
    return static_cast<int32_t>(sum);
}


// ─── PyCodeObject::lineAt / columnAt ─────────────────────────────────────────

int32_t PyCodeObject::lineAt(uint32_t offset) const {
    for (const auto& e : lineTable) {
        if (offset >= e.startOffset && offset < e.endOffset)
            return e.line;
    }
    return co_firstlineno;
}

int32_t PyCodeObject::columnAt(uint32_t offset) const {
    for (const auto& e : lineTable) {
        if (offset >= e.startOffset && offset < e.endOffset)
            return e.column;
    }
    return -1;
}

// ─── decodeLnotab (Python ≤ 3.9) ─────────────────────────────────────────────

std::vector<LineEntry> decodeLnotab(
        const std::vector<uint8_t>& lnotab,
        int32_t firstLineno,
        uint32_t codeLen) {
    std::vector<LineEntry> result;
    if (lnotab.empty()) {
        if (codeLen > 0) {
            result.push_back({0, codeLen, firstLineno, -1, -1, -1});
        }
        return result;
    }

    uint32_t byteOffset = 0;
    int32_t  line       = firstLineno;

    // lnotab is a sequence of (byte_increment, line_increment) pairs
    for (size_t i = 0; i + 1 < lnotab.size(); i += 2) {
        uint8_t byteInc = lnotab[i];
        uint8_t lineInc = lnotab[i + 1];

        uint32_t start = byteOffset;
        byteOffset += byteInc;

        if (!result.empty() && byteOffset > start) {
            result.back().endOffset = byteOffset;
        }

        if (lineInc != 0 || i == 0) {
            // If this is the first entry or line changes, add a new entry
            if (result.empty() || byteOffset > result.back().startOffset) {
                LineEntry e;
                e.startOffset = byteOffset;
                e.endOffset   = byteOffset; // will be updated
                e.line        = addLineDelta(line, lineInc);
                e.column      = -1;
                result.push_back(e);
            }
            line = addLineDelta(line, lineInc);
        }
    }

    // Simpler pass: start fresh
    result.clear();
    byteOffset = 0;
    line       = firstLineno;

    uint32_t segStart = 0;
    for (size_t i = 0; i + 1 < lnotab.size(); i += 2) {
        uint8_t byteInc = lnotab[i];
        uint8_t lineInc = lnotab[i + 1];

        if (lineInc != 0) {
            // Close current segment and open new
            if (byteInc > 0 || result.empty()) {
                result.push_back({segStart, byteOffset + byteInc, line, -1, -1, -1});
                segStart = byteOffset + byteInc;
                line     = addLineDelta(line, lineInc);
            } else {
                // byteInc == 0: same offset, just bump line
                line = addLineDelta(line, lineInc);
            }
        }
        byteOffset += byteInc;
    }
    // Last segment
    if (byteOffset < codeLen || result.empty()) {
        result.push_back({segStart, codeLen, line, -1, -1, -1});
    } else if (!result.empty()) {
        result.back().endOffset = codeLen;
    }

    return result;
}

// ─── decodeLnotab310 (Python 3.10) ───────────────────────────────────────────

std::vector<LineEntry> decodeLnotab310(
        const std::vector<uint8_t>& linetable,
        int32_t firstLineno,
        uint32_t codeLen) {
    // Python 3.10 linetable: each entry is 2 bytes
    //   byte 0: (start_offset_delta << 3) | flags? ... actually in 3.10 the format
    //           was: pairs (bytecode_delta, line_delta) but both can be > 255
    //   Actually 3.10 is still basically lnotab format but with wider fields in some cases.
    //   The real 3.10 format uses the same lnotab with a slight change to allow
    //   larger line deltas. We treat it the same as ≤ 3.9.
    return decodeLnotab(linetable, firstLineno, codeLen);
}

// ─── decodeLnotab311 (Python 3.11+ PEP 657) ──────────────────────────────────

// Helper: decode unsigned ULEB128
//
// The continuation bit is set by the file, so the shift is driven by the input
// too: a run of 0xFF bytes used to push it past the width of the result, which
// is undefined. A uint32_t holds at most five 7-bit groups, so groups beyond
// that carry no representable bits -- they are consumed to keep the caller's
// position on an entry boundary, but contribute nothing.
static uint32_t readUleb128(const uint8_t* data, size_t size, size_t& pos) {
    uint32_t result = 0;
    unsigned shift = 0;
    while (pos < size) {
        uint8_t b = data[pos++];
        if (shift < 32)
            result |= static_cast<uint32_t>(b & 0x7F) << shift;
        if ((b & 0x80) == 0) break;
        shift += 7;
    }
    return result;
}

std::vector<LineEntry> decodeLnotab311(
        const std::vector<uint8_t>& linetable,
        int32_t firstLineno,
        uint32_t codeLen) {
    // Python 3.11+ co_linetable (PEP 657):
    // Each entry covers 2n instructions (n = number of code units in the entry).
    //
    // Entry format:
    //   byte 0: code byte
    //       bits 7-3: entry_size = (code >> 3) + 1  (number of 2-byte instructions)
    //       bits 2-0: code_type
    //           0 = short form (line only, small delta)
    //           1 = one-line form  (line, column info)
    //           2 = no-col form    (line delta, no column)
    //           3 = long form
    //           9 = no-line form   (no line info for this range)
    //           10 = internal/restart entry
    //
    // This is complex; implement a best-effort decoder.

    std::vector<LineEntry> result;
    const uint8_t* data = linetable.data();
    size_t size = linetable.size();
    size_t pos = 0;

    uint32_t offset = 0;
    int32_t  line   = firstLineno;
    int32_t  col    = -1;
    int32_t  endLine= -1;
    int32_t  endCol = -1;

    while (pos < size && offset < codeLen) {
        if (pos >= size) break;
        uint8_t code = data[pos++];

        bool hasFlag = (code & 0x80) != 0;
        uint8_t entry = code & 0x7F;

        // In 3.11+ each instruction is 2 bytes (one "word")
        // entry_size = (entry >> 3) + 1 code units
        uint32_t numCodeUnits = ((entry >> 3) & 0x0F) + 1;
        uint8_t  codeType     = entry & 0x07;

        uint32_t byteSpan = numCodeUnits * 2;

        bool noLineInfo = false;

        switch (codeType) {
        case 0: {
            // Short form: line delta in [0..2], col in [0..64].
            // Two bytes follow, so two must be available -- the check used to
            // ask for one and read past the end of a truncated table.
            if (pos + 2 > size) goto done;
            int8_t lineDelta = static_cast<int8_t>(data[pos++]);
            int8_t colStart  = static_cast<int8_t>(data[pos++]);
            line = addLineDelta(line, lineDelta);
            col   = colStart;
            endLine = line;
            endCol  = -1;
            break;
        }
        case 1: {
            // One-line form: three bytes follow (line delta, start and end
            // column), not two.
            if (pos + 3 > size) goto done;
            int8_t lineDelta  = static_cast<int8_t>(data[pos++]);
            int8_t colStart   = static_cast<int8_t>(data[pos++]);
            int8_t colEnd     = static_cast<int8_t>(data[pos++]);
            (void)colEnd;
            line = addLineDelta(line, lineDelta);
            col   = colStart;
            endLine = line;
            endCol  = colEnd >= 0 ? colEnd : -1;
            break;
        }
        case 2: {
            // No-col form: line delta, no column
            if (pos + 1 > size) goto done;
            int8_t lineDelta = static_cast<int8_t>(data[pos++]);
            line = addLineDelta(line, lineDelta);
            col   = -1;
            endLine = line;
            endCol  = -1;
            break;
        }
        case 3: {
            // Long form: slines, elines, scol, ecol
            if (pos + 4 > size) goto done;
            int8_t sLine = static_cast<int8_t>(data[pos++]);
            int8_t eLine = static_cast<int8_t>(data[pos++]);
            int8_t sCol  = static_cast<int8_t>(data[pos++]);
            int8_t eCol  = static_cast<int8_t>(data[pos++]);
            line    = addLineDelta(line, sLine);
            endLine = addLineDelta(line, eLine);
            col     = sCol;
            endCol  = eCol;
            break;
        }
        case 9:  // No line info
        case 10: // Internal
            noLineInfo = true;
            break;
        default:
            noLineInfo = true;
            break;
        }

        if (!noLineInfo && line > 0) {
            result.push_back({offset, offset + byteSpan, line, endLine, col, endCol});
        }
        offset += byteSpan;
        (void)hasFlag;
    }
done:
    // Ensure last entry covers to codeLen
    if (!result.empty())
        result.back().endOffset = codeLen;
    else if (codeLen > 0)
        result.push_back({0, codeLen, firstLineno, -1, -1, -1});

    return result;
}

// ─── decodeExceptionTable ────────────────────────────────────────────────────

std::vector<ExceptionEntry> decodeExceptionTable(
        const std::vector<uint8_t>& table) {
    // Python 3.11+ exception table format:
    // Variable-length ULEB128-encoded entries:
    //   start:  code offset of the try block start
    //   length: byte length of the try block
    //   target: code offset of the exception handler
    //   dl:     (depth << 1) | lasti flag

    std::vector<ExceptionEntry> result;
    const uint8_t* data = table.data();
    size_t size = table.size();
    size_t pos = 0;

    while (pos < size) {
        uint32_t start  = readUleb128(data, size, pos);
        if (pos >= size) break;
        uint32_t length = readUleb128(data, size, pos);
        if (pos >= size) break;
        uint32_t target = readUleb128(data, size, pos);
        if (pos >= size) break;
        uint32_t dl     = readUleb128(data, size, pos);

        ExceptionEntry e;
        e.start  = start;
        e.end    = start + length;
        e.target = target;
        e.depth  = dl >> 1;
        e.lasti  = (dl & 1) != 0;
        result.push_back(e);
    }
    return result;
}

} // namespace pyc_parser
} // namespace retdec
