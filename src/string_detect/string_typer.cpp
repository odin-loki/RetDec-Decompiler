/**
 * @file src/string_detect/string_typer.cpp
 * @brief String classification and encoding detection.
 *
 * ## Classification order
 *
 * 1. UTF-16LE / UTF-16BE  (detected first: alternating-zero-byte signature)
 * 2. Pascal (u8-length-prefixed, max 255 chars)
 * 3. MSVC length-prefixed (u32-length-prefixed, up to 64 KiB)
 * 4. C NUL-terminated ASCII / UTF-8 / Latin-1
 *
 * We attempt each in turn and return the first that succeeds.
 *
 * ## Encoding detection algorithm
 *
 * We analyse up to 512 bytes using byte-frequency counters:
 *
 *   all_ascii     : every byte in [0x09,0x0D] ∪ [0x20,0x7E]
 *   has_high      : any byte in [0x80,0xFF]
 *   valid_utf8    : byte sequences satisfy UTF-8 continuation rules
 *   utf16le_hint  : even-indexed bytes mostly 0x00, odd-indexed mostly non-zero
 *   utf16be_hint  : odd-indexed bytes mostly 0x00, even-indexed mostly non-zero
 *
 * Decision tree:
 *   if utf16le_hint and !utf16be_hint → UTF-16LE
 *   if utf16be_hint and !utf16le_hint → UTF-16BE
 *   if all_ascii → ASCII
 *   if valid_utf8 and has_high → UTF-8
 *   if has_high and not valid_utf8 → Latin-1
 *   else → Unknown
 */

#include "retdec/string_detect/string_detect.h"

#include <algorithm>
#include <cassert>
#include <cstring>
#include <optional>
#include <string>

namespace retdec {
namespace string_detect {

// ─── IBinaryView helpers ──────────────────────────────────────────────────────

uint64_t IBinaryView::readPointer(uint64_t vma) const {
    uint8_t buf[8] = {};
    std::size_t n = readBytes(vma, buf, pointerWidth());
    if (n < pointerWidth()) return 0;
    if (pointerWidth() == 4) {
        return (uint64_t)buf[0] | ((uint64_t)buf[1]<<8) |
               ((uint64_t)buf[2]<<16) | ((uint64_t)buf[3]<<24);
    }
    uint64_t v = 0;
    for (int i=7; i>=0; --i) v = (v<<8) | buf[i];
    return v;
}

// ─── Utility names ────────────────────────────────────────────────────────────

const char* encodingName(EncodingKind e) noexcept {
    switch (e) {
    case EncodingKind::ASCII:   return "ASCII";
    case EncodingKind::UTF8:    return "UTF-8";
    case EncodingKind::Latin1:  return "Latin-1";
    case EncodingKind::UTF16LE: return "UTF-16LE";
    case EncodingKind::UTF16BE: return "UTF-16BE";
    case EncodingKind::UTF32LE: return "UTF-32LE";
    default:                    return "Unknown";
    }
}

const char* stringKindName(StringKind k) noexcept {
    switch (k) {
    case StringKind::CNulTerminated: return "C-string";
    case StringKind::Pascal:         return "Pascal";
    case StringKind::LengthPrefixed: return "LengthPrefixed";
    case StringKind::Wide:           return "Wide";
    case StringKind::SSOInline:      return "SSO-Inline";
    case StringKind::TableEntry:     return "TableEntry";
    }
    return "?";
}

std::string StringLiteral::debugStr() const {
    return std::string(stringKindName(kind)) + " [" + encodingName(encoding) + "] "
         + "\"" + value + "\" @ 0x" + [&]{
               char buf[32]; std::snprintf(buf,sizeof(buf),"%llx",(unsigned long long)address);
               return std::string(buf); }()
         + " (" + std::to_string(charCount) + " chars)";
}

// ─── Encoding detector ────────────────────────────────────────────────────────

EncodingResult detectEncoding(const uint8_t* buf, std::size_t len) noexcept {
    if (len == 0) return { EncodingKind::Unknown, 0.0f };

    // ── UTF-16LE / UTF-16BE detection (alternating zeros) ─────────────────────
    // Check first 64 bytes (or whole buffer if shorter)
    std::size_t checkLen = std::min(len, std::size_t(64));
    int zeroEven=0, zeroOdd=0, nonzeroEven=0, nonzeroOdd=0;
    for (std::size_t i=0; i+1<checkLen; i+=2) {
        if (buf[i]   == 0) ++zeroEven; else ++nonzeroEven;
        if (buf[i+1] == 0) ++zeroOdd;  else ++nonzeroOdd;
    }
    int pairs = (int)(checkLen/2);
    if (pairs >= 2) {
        float leScore = (zeroOdd  + nonzeroEven) / (float)(pairs*2);
        float beScore = (zeroEven + nonzeroOdd)  / (float)(pairs*2);
        if (leScore > 0.80f && beScore < 0.50f)
            return { EncodingKind::UTF16LE, leScore };
        if (beScore > 0.80f && leScore < 0.50f)
            return { EncodingKind::UTF16BE, beScore };
    }

    // ── Byte classification ────────────────────────────────────────────────────
    bool allAscii     = true;
    bool hasHigh      = false;
    bool validUtf8    = true;
    std::size_t i = 0;
    while (i < len) {
        uint8_t c = buf[i];
        if (c == 0) break; // NUL terminator — stop
        if (c < 0x09 || (c > 0x0D && c < 0x20) || c == 0x7F) {
            // Control character (not tab/newline/CR) → not printable ASCII
            allAscii = false;
        }
        if (c >= 0x80) {
            hasHigh = true;
            allAscii = false;
            // Check UTF-8 continuation
            int extra = 0;
            if      ((c & 0xE0)==0xC0) extra=1;
            else if ((c & 0xF0)==0xE0) extra=2;
            else if ((c & 0xF8)==0xF0) extra=3;
            else { validUtf8=false; ++i; continue; }
            for (int e=0; e<extra; ++e) {
                if (i+1+e >= len || (buf[i+1+e]&0xC0)!=0x80) { validUtf8=false; break; }
            }
            i += 1+extra;
            continue;
        }
        ++i;
    }

    if (allAscii)                    return { EncodingKind::ASCII,  1.0f };
    if (hasHigh && validUtf8)        return { EncodingKind::UTF8,   0.9f };
    if (hasHigh)                     return { EncodingKind::Latin1, 0.7f };
    return { EncodingKind::Unknown, 0.0f };
}

// ─── Printability helpers ─────────────────────────────────────────────────────

static bool isPrintableAscii(uint8_t c) {
    return (c >= 0x20 && c <= 0x7E) ||
           c == '\t' || c == '\n' || c == '\r';
}
static bool isPrintableLatin1(uint8_t c) {
    return isPrintableAscii(c) || (c >= 0xA0);
}
static bool isPrintableUtf8Lead(uint8_t c) {
    return isPrintableAscii(c) || (c >= 0xC2 && c <= 0xF4);
}

// ─── UTF-16LE/BE reader ───────────────────────────────────────────────────────

static std::optional<std::string>
readUtf16(const IBinaryView& view, uint64_t vma, bool le, std::size_t maxBytes) {
    std::string result;
    result.reserve(64);
    uint8_t buf[2];
    for (std::size_t i=0; i<maxBytes-1; i+=2) {
        if (view.readBytes(vma+i, buf, 2) < 2) return std::nullopt;
        uint16_t wc = le ? (uint16_t)(buf[0] | (buf[1]<<8))
                         : (uint16_t)((buf[0]<<8) | buf[1]);
        if (wc == 0) {
            // NUL terminator
            if (result.empty()) return std::nullopt;
            return result;
        }
        // Convert to UTF-8 (BMP only for now)
        if (wc < 0x80) {
            result += (char)wc;
        } else if (wc < 0x800) {
            result += (char)(0xC0 | (wc>>6));
            result += (char)(0x80 | (wc&0x3F));
        } else {
            result += (char)(0xE0 | (wc>>12));
            result += (char)(0x80 | ((wc>>6)&0x3F));
            result += (char)(0x80 | (wc&0x3F));
        }
        // Sanity: reject obvious non-text code points
        if (wc < 0x20 && wc != '\t' && wc != '\n' && wc != '\r')
            return std::nullopt;
    }
    return std::nullopt; // unterminated within maxBytes
}

// ─── Main classifier ──────────────────────────────────────────────────────────

std::optional<StringLiteral> typeString(const IBinaryView& view,
                                         uint64_t            vma,
                                         std::size_t         maxBytes)
{
    if (!view.isMapped(vma)) return std::nullopt;

    // Read a working buffer (up to maxBytes or 4096)
    std::size_t bufSz = std::min(maxBytes, std::size_t(4096));
    std::vector<uint8_t> buf(bufSz, 0);
    std::size_t got = view.readBytes(vma, buf.data(), bufSz);
    if (got < 2) return std::nullopt;

    // ── 1. Try UTF-16LE ────────────────────────────────────────────────────────
    {
        // Check first two bytes: if byte[1]==0 and byte[0] is printable ASCII,
        // very likely UTF-16LE
        if (got >= 4 && buf[1]==0 && buf[3]==0 && isPrintableAscii(buf[0]) && isPrintableAscii(buf[2])) {
            auto s = readUtf16(view, vma, true, bufSz);
            if (s && s->size() >= 2) {
                StringLiteral sl;
                sl.address     = vma;
                sl.value       = std::move(*s);
                sl.charCount   = sl.value.size(); // approx (UTF-8 chars)
                sl.byteLength  = sl.charCount*2 + 2; // wide chars + NUL
                sl.kind        = StringKind::Wide;
                sl.encoding    = EncodingKind::UTF16LE;
                return sl;
            }
        }
    }

    // ── 2. Try UTF-16BE ────────────────────────────────────────────────────────
    {
        if (got >= 4 && buf[0]==0 && buf[2]==0 && isPrintableAscii(buf[1]) && isPrintableAscii(buf[3])) {
            auto s = readUtf16(view, vma, false, bufSz);
            if (s && s->size() >= 2) {
                StringLiteral sl;
                sl.address   = vma;
                sl.value     = std::move(*s);
                sl.charCount = sl.value.size();
                sl.byteLength= sl.charCount*2+2;
                sl.kind      = StringKind::Wide;
                sl.encoding  = EncodingKind::UTF16BE;
                return sl;
            }
        }
    }

    // ── 3. Try Pascal string (u8 length prefix) ────────────────────────────────
    {
        uint8_t plen = buf[0];
        if (plen >= 2 && plen <= 255 && (std::size_t)plen+1 <= got) {
            bool ok = true;
            for (uint8_t i=1; i<=(plen); ++i) {
                if (!isPrintableLatin1(buf[i])) { ok=false; break; }
            }
            if (ok) {
                auto enc = detectEncoding(buf.data()+1, plen);
                StringLiteral sl;
                sl.address   = vma;
                sl.byteLength= plen+1;
                sl.charCount = plen;
                sl.kind      = StringKind::Pascal;
                sl.encoding  = enc.kind;
                sl.value     = std::string(reinterpret_cast<const char*>(buf.data()+1), plen);
                return sl;
            }
        }
    }

    // ── 4. Try MSVC/COM length-prefixed (u32 length prefix) ───────────────────
    {
        if (got >= 6) {
            uint32_t lplen = (uint32_t)buf[0] | ((uint32_t)buf[1]<<8) |
                             ((uint32_t)buf[2]<<16) | ((uint32_t)buf[3]<<24);
            if (lplen >= 2 && lplen <= 65535 && lplen+4 <= bufSz) {
                bool ok = true;
                for (uint32_t i=0; i<lplen; ++i) {
                    if (!isPrintableLatin1(buf[4+i])) { ok=false; break; }
                }
                // Also need NUL at end
                if (ok && 4+lplen < got && buf[4+lplen]==0) {
                    auto enc = detectEncoding(buf.data()+4, lplen);
                    StringLiteral sl;
                    sl.address   = vma;
                    sl.byteLength= 4+lplen+1;
                    sl.charCount = lplen;
                    sl.kind      = StringKind::LengthPrefixed;
                    sl.encoding  = enc.kind;
                    sl.value     = std::string(reinterpret_cast<const char*>(buf.data()+4), lplen);
                    return sl;
                }
            }
        }
    }

    // ── 5. C NUL-terminated string ────────────────────────────────────────────
    {
        // Find NUL terminator
        std::size_t nulPos = got;
        for (std::size_t i=0; i<got; ++i) {
            if (buf[i]==0) { nulPos=i; break; }
        }
        if (nulPos < 2) return std::nullopt; // too short

        // Validate: all bytes printable
        bool ok = true;
        for (std::size_t i=0; i<nulPos; ++i) {
            if (!isPrintableUtf8Lead(buf[i]) && !isPrintableLatin1(buf[i])) {
                ok=false; break;
            }
        }
        if (!ok) return std::nullopt;

        auto enc = detectEncoding(buf.data(), nulPos);
        if (enc.kind == EncodingKind::Unknown && nulPos < 4)
            return std::nullopt; // too short and unrecognised

        StringLiteral sl;
        sl.address   = vma;
        sl.byteLength= nulPos+1;
        sl.charCount = nulPos;
        sl.kind      = StringKind::CNulTerminated;
        sl.encoding  = (enc.kind==EncodingKind::Unknown) ? EncodingKind::ASCII : enc.kind;
        sl.value     = std::string(reinterpret_cast<const char*>(buf.data()), nulPos);
        return sl;
    }
}

} // namespace string_detect
} // namespace retdec
