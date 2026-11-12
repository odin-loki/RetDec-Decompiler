// forge::os::text: UTF-8/16/32 conversion and validation.
#include "internal.hpp"

namespace forge::os::text {
cp_t decode_utf8(std::string_view s, std::size_t& i, bool* ok) {
    auto u = [&](std::size_t k) { return (unsigned char)s[k]; };
    unsigned char b = u(i);
    if (ok) *ok = true;
    if (b < 0x80) { ++i; return b; }
    int n; cp_t c; unsigned char lo = 0x80, hi = 0xBF;
    if (b >= 0xC2 && b <= 0xDF) { n = 1; c = b & 0x1F; }
    else if (b >= 0xE0 && b <= 0xEF) { n = 2; c = b & 0x0F; if (b == 0xE0) lo = 0xA0; if (b == 0xED) hi = 0x9F; }
    else if (b >= 0xF0 && b <= 0xF4) { n = 3; c = b & 0x07; if (b == 0xF0) lo = 0x90; if (b == 0xF4) hi = 0x8F; }
    else { ++i; if (ok) *ok = false; return replacement_char; }
    std::size_t j = i + 1;
    for (int k = 0; k < n; ++k, ++j) {
        if (j >= s.size()) { i = j; if (ok) *ok = false; return replacement_char; }
        unsigned char x = u(j);
        if (x < lo || x > hi) { i = j; if (ok) *ok = false; return replacement_char; }   // maximal subpart
        lo = 0x80; hi = 0xBF; c = (c << 6) | (x & 0x3F);
    }
    i = j; return c;
}
void append_utf8(std::string& o, cp_t c) {
    if (c >= 0xD800 && c <= 0xDFFF) c = replacement_char;
    if (c < 0x80) o += char(c);
    else if (c < 0x800) { o += char(0xC0 | (c >> 6)); o += char(0x80 | (c & 0x3F)); }
    else if (c < 0x10000) { o += char(0xE0 | (c >> 12)); o += char(0x80 | ((c >> 6) & 0x3F)); o += char(0x80 | (c & 0x3F)); }
    else if (c <= 0x10FFFF) { o += char(0xF0 | (c >> 18)); o += char(0x80 | ((c >> 12) & 0x3F)); o += char(0x80 | ((c >> 6) & 0x3F)); o += char(0x80 | (c & 0x3F)); }
    else append_utf8(o, replacement_char);
}
void append_utf16(std::u16string& o, cp_t c) {
    if (c >= 0x10000 && c <= 0x10FFFF) { c -= 0x10000; o += char16_t(0xD800 + (c >> 10)); o += char16_t(0xDC00 + (c & 0x3FF)); }
    else if (c > 0x10FFFF || (c >= 0xD800 && c <= 0xDFFF)) o += char16_t(replacement_char);
    else o += char16_t(c);
}
std::string encode_utf8(std::u32string_view s) { std::string o; o.reserve(s.size()); for (auto c : s) append_utf8(o, c); return o; }
std::u32string utf8_to_utf32(std::string_view s) {
    std::u32string o; o.reserve(s.size()); std::size_t i = 0; while (i < s.size()) o += decode_utf8(s, i); return o;
}
std::size_t validate_utf8(std::string_view s) {
    std::size_t i = 0;
    while (i < s.size()) {
        if ((unsigned char)s[i] < 0x80) { ++i; continue; }
        std::size_t st = i; bool ok; decode_utf8(s, i, &ok); if (!ok) return st;
    }
    return std::string_view::npos;
}
std::string sanitize_utf8(std::string_view s) {
    if (is_valid_utf8(s)) return std::string(s);
    std::string o; o.reserve(s.size()); std::size_t i = 0; while (i < s.size()) append_utf8(o, decode_utf8(s, i)); return o;
}
std::size_t count_code_points(std::string_view s) { std::size_t n = 0, i = 0; while (i < s.size()) { decode_utf8(s, i); ++n; } return n; }
result<std::u16string> utf8_to_utf16(std::string_view s) {
    result<std::u16string> r; r.value.reserve(s.size()); std::size_t i = 0;
    while (i < s.size()) {
        std::size_t st = i; bool ok; cp_t c = decode_utf8(s, i, &ok);
        if (!ok) { r.err = {EILSEQ, "utf8_to_utf16: invalid UTF-8 at byte " + std::to_string(st)}; return r; }
        append_utf16(r.value, c);
    }
    return r;
}
static std::string u16_to_u8(std::u16string_view s, bool strict, error* e) {
    std::string o; o.reserve(s.size());
    for (std::size_t i = 0; i < s.size(); ++i) {
        cp_t c = s[i];
        if (c >= 0xD800 && c <= 0xDBFF && i + 1 < s.size() && s[i + 1] >= 0xDC00 && s[i + 1] <= 0xDFFF) {
            c = 0x10000 + ((c - 0xD800) << 10) + (s[i + 1] - 0xDC00); ++i;
        } else if (c >= 0xD800 && c <= 0xDFFF) {
            if (strict) { *e = {EILSEQ, "utf16_to_utf8: unpaired surrogate at unit " + std::to_string(i)}; return {}; }
            c = replacement_char;
        }
        append_utf8(o, c);
    }
    return o;
}
result<std::string> utf16_to_utf8(std::u16string_view s) { result<std::string> r; r.value = u16_to_u8(s, true, &r.err); return r; }
std::string utf16_to_utf8_lossy(std::u16string_view s) { return u16_to_u8(s, false, nullptr); }
result<std::string> utf32_to_utf8(std::u32string_view s) {
    result<std::string> r;
    for (std::size_t i = 0; i < s.size(); ++i) {
        if (s[i] > 0x10FFFF || (s[i] >= 0xD800 && s[i] <= 0xDFFF)) { r.err = {EILSEQ, "utf32_to_utf8: invalid code point at " + std::to_string(i)}; return r; }
        append_utf8(r.value, s[i]);
    }
    return r;
}
}  // namespace forge::os::text
