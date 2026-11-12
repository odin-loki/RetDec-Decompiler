// forge::os::text: legacy charsets <-> UTF-8 (WHATWG Encoding Standard algorithms), BOMs and sniffing.
#include "internal.hpp"
#include "encoding_data.inc"
#include "../../../core/include/forge/core/memory.hpp"
#include <algorithm>
#include <cerrno>
#include <mutex>
#include <unordered_map>

namespace forge::os::text {
using namespace detail;
namespace {
enum class kind { utf8, utf16le, utf16be, utf32le, utf32be, ascii, latin1, single, sjis, eucjp, gbk, gb18030, big5, euckr, iso2022jp };
struct cs { kind k; int table = -1; const char* name; };
std::string lower(std::string_view s) { std::string r; for (char c : s) r += (c >= 'A' && c <= 'Z') ? char(c + 32) : c; return r; }
std::string trim(std::string_view s) { while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) s.remove_prefix(1); while (!s.empty() && (s.back() == ' ' || s.back() == '\t')) s.remove_suffix(1); return std::string(s); }

const std::vector<std::pair<std::string, std::string>>& labels() {
    static const std::vector<std::pair<std::string, std::string>> L = {
        {"utf8", "utf-8"}, {"unicode-1-1-utf-8", "utf-8"}, {"utf-16", "utf-16le"}, {"ucs-2", "utf-16le"}, {"unicode", "utf-16le"},
        {"utf-16le", "utf-16le"}, {"utf-16be", "utf-16be"}, {"unicodefffe", "utf-16be"}, {"utf-32", "utf-32le"}, {"utf-32le", "utf-32le"}, {"utf-32be", "utf-32be"},
        {"ascii", "us-ascii"}, {"us-ascii", "us-ascii"}, {"ansi_x3.4-1968", "us-ascii"}, {"646", "us-ascii"},
        {"latin1", "iso-8859-1"}, {"l1", "iso-8859-1"}, {"iso8859-1", "iso-8859-1"}, {"iso_8859-1", "iso-8859-1"}, {"iso-8859-1", "iso-8859-1"}, {"cp819", "iso-8859-1"},
        {"cp1252", "windows-1252"}, {"x-cp1252", "windows-1252"}, {"ansi", "windows-1252"},
        // WHATWG: every ISO-8859-9 (Latin-5) label decodes as windows-1254, its superset.
        {"iso-8859-9", "windows-1254"}, {"iso8859-9", "windows-1254"}, {"iso88599", "windows-1254"}, {"iso_8859-9", "windows-1254"}, {"iso_8859-9:1989", "windows-1254"},
        {"iso-ir-148", "windows-1254"}, {"csisolatin5", "windows-1254"}, {"latin5", "windows-1254"}, {"l5", "windows-1254"}, {"x-cp1254", "windows-1254"},
        {"cp874", "windows-874"}, {"tis-620", "windows-874"}, {"iso-8859-11", "windows-874"},
        {"latin2", "iso-8859-2"}, {"latin3", "iso-8859-3"}, {"latin4", "iso-8859-4"}, {"cyrillic", "iso-8859-5"}, {"arabic", "iso-8859-6"},
        {"greek", "iso-8859-7"}, {"hebrew", "iso-8859-8"}, {"latin6", "iso-8859-10"}, {"latin9", "iso-8859-15"}, {"latin10", "iso-8859-16"},
        {"koi8", "koi8-r"}, {"koi", "koi8-r"}, {"koi8_r", "koi8-r"}, {"cp866", "ibm866"}, {"866", "ibm866"}, {"mac", "macintosh"}, {"x-mac-roman", "macintosh"}, {"macroman", "macintosh"},
        {"cp437", "ibm437"}, {"437", "ibm437"}, {"cp850", "ibm850"}, {"850", "ibm850"},
        {"sjis", "shift_jis"}, {"shift-jis", "shift_jis"}, {"ms_kanji", "shift_jis"}, {"windows-31j", "shift_jis"}, {"cp932", "shift_jis"}, {"x-sjis", "shift_jis"},
        {"euc-jp", "euc-jp"}, {"iso-2022-jp", "iso-2022-jp"}, {"csiso2022jp", "iso-2022-jp"}, {"jis", "iso-2022-jp"}, {"eucjp", "euc-jp"}, {"x-euc-jp", "euc-jp"},
        {"gbk", "gbk"}, {"gb2312", "gbk"}, {"cp936", "gbk"}, {"x-gbk", "gbk"}, {"euc-cn", "gbk"}, {"windows-936", "gbk"}, {"gb18030", "gb18030"},
        {"big5", "big5"}, {"big5-hkscs", "big5"}, {"cn-big5", "big5"}, {"x-x-big5", "big5"}, {"cp950", "big5"},
        {"euc-kr", "euc-kr"}, {"cp949", "euc-kr"}, {"ks_c_5601-1987", "euc-kr"}, {"windows-949", "euc-kr"}, {"korean", "euc-kr"}, {"uhc", "euc-kr"}};
    return L;
}
std::optional<cs> lookup(std::string_view label) {
    auto l = lower(trim(label));
    for (auto& [a, b] : labels()) if (l == a) { l = b; break; }
    if (l.starts_with("cp125") && l.size() == 6) l = "windows-" + l.substr(2);
    if (l.starts_with("iso8859-")) l = "iso-" + l.substr(3);
    if (l == "utf-8") return cs{kind::utf8, -1, "utf-8"};
    if (l == "utf-16le") return cs{kind::utf16le, -1, "utf-16le"};
    if (l == "utf-16be") return cs{kind::utf16be, -1, "utf-16be"};
    if (l == "utf-32le") return cs{kind::utf32le, -1, "utf-32le"};
    if (l == "utf-32be") return cs{kind::utf32be, -1, "utf-32be"};
    if (l == "us-ascii") return cs{kind::ascii, -1, "us-ascii"};
    if (l == "iso-8859-1") return cs{kind::latin1, -1, "iso-8859-1"};
    if (l == "shift_jis") return cs{kind::sjis, -1, "shift_jis"};
    if (l == "euc-jp") return cs{kind::eucjp, -1, "euc-jp"};
    if (l == "iso-2022-jp") return cs{kind::iso2022jp, -1, "iso-2022-jp"};
    if (l == "gbk") return cs{kind::gbk, -1, "gbk"};
    if (l == "gb18030") return cs{kind::gb18030, -1, "gb18030"};
    if (l == "big5") return cs{kind::big5, -1, "big5"};
    if (l == "euc-kr") return cs{kind::euckr, -1, "euc-kr"};
    for (int i = 0; i < int(std::size(encdata::single_byte_names)); ++i) if (l == encdata::single_byte_names[i]) return cs{kind::single, i, encdata::single_byte_names[i]};
    return std::nullopt;
}
cp_t gb_range_cp(unsigned ptr) {
    if ((ptr > 39419 && ptr < 189000) || ptr > 1237575) return 0;
    if (ptr == 7457) return 0xE7C7;
    if (ptr >= 189000) return 0x10000 + ptr - 189000;
    unsigned best = 0, bcp = 0;
    for (std::size_t i = 0; i < std::size(encdata::gb18030_ranges); i += 2) if (encdata::gb18030_ranges[i] <= ptr) { best = encdata::gb18030_ranges[i]; bcp = encdata::gb18030_ranges[i + 1]; } else break;
    return bcp + ptr - best;
}
unsigned gb_range_ptr(cp_t c) {
    if (c == 0xE7C7) return 7457;
    if (c >= 0x10000) return 189000 + c - 0x10000;
    unsigned best = 0, bcp = 0;
    for (std::size_t i = 0; i < std::size(encdata::gb18030_ranges); i += 2) if (encdata::gb18030_ranges[i + 1] <= c) { best = encdata::gb18030_ranges[i]; bcp = encdata::gb18030_ranges[i + 1]; } else break;
    return best + c - bcp;
}
// Reverse maps for the multi-byte encoders, built once on first use and charged to forge::mem.
struct reverse_maps {
    std::unordered_map<cp_t, unsigned> jis, sjis, gb, big5, kr;
    std::vector<std::unordered_map<cp_t, unsigned char>> sb;
    mem::lease lease;
};
const reverse_maps& rev() {
    static reverse_maps& R = *new reverse_maps; static std::once_flag once;   // never destroyed: outlives forge::mem at exit
    std::call_once(once, [] {
        auto fill = [](auto& m, const auto& arr, auto keep) { for (unsigned i = 0; i < std::size(arr); ++i) if (arr[i] && keep(i)) m.emplace(cp_t(arr[i]), i); };
        fill(R.jis, encdata::jis0208, [](unsigned) { return true; });
        fill(R.sjis, encdata::jis0208, [](unsigned p) { return p < 8272 || p > 8835; });
        fill(R.gb, encdata::gb18030, [](unsigned) { return true; });
        fill(R.kr, encdata::euc_kr, [](unsigned) { return true; });
        for (unsigned i = 0; i < std::size(encdata::big5); ++i) {
            cp_t c = encdata::big5[i]; if (!c || i < (0xA1 - 0x81) * 157) continue;
            bool last = c == 0x2550 || c == 0x255E || c == 0x2561 || c == 0x256A || c == 0x5341 || c == 0x5345;
            if (last) R.big5[c] = i; else R.big5.emplace(c, i);
        }
        R.sb.resize(std::size(encdata::single_byte_names));
        for (std::size_t t = 0; t < R.sb.size(); ++t) for (unsigned i = 0; i < 128; ++i) if (auto c = encdata::single_byte[t * 128 + i]) R.sb[t].emplace(cp_t(c), (unsigned char)(0x80 + i));
        std::size_t n = R.jis.size() + R.sjis.size() + R.gb.size() + R.big5.size() + R.kr.size() + R.sb.size() * 128;
        R.lease = mem::host_budget().try_reserve(n * 32, "text");
    });
    return R;
}
}  // namespace

std::vector<std::string> charsets() {
    std::vector<std::string> r = {"utf-8", "utf-16le", "utf-16be", "utf-32le", "utf-32be", "us-ascii", "iso-8859-1", "shift_jis", "euc-jp", "iso-2022-jp", "gbk", "gb18030", "big5", "euc-kr"};
    for (auto n : encdata::single_byte_names) r.push_back(n);
    return r;
}
std::optional<std::string> canonical_charset(std::string_view label) { if (auto c = lookup(label)) return std::string(c->name); return std::nullopt; }

result<std::string> decode(std::string_view b, std::string_view charset, on_error onerr) {
    result<std::string> r;
    auto c = lookup(charset);
    if (!c) { r.err = {EINVAL, "decode: unknown charset '" + std::string(charset) + "'"}; return r; }
    std::string& o = r.value; o.reserve(b.size() + b.size() / 2);
    bool failed = false; std::size_t fail_at = 0;
    auto bad = [&](std::size_t at) { if (onerr == on_error::fail) { if (!failed) { failed = true; fail_at = at; } } else if (onerr == on_error::replace) append_utf8(o, replacement_char); };
    auto u = [&](std::size_t i) { return (unsigned char)b[i]; };
    const std::size_t n = b.size();
    switch (c->k) {
    case kind::utf8: {
        std::size_t i = 0; if (n >= 3 && b.substr(0, 3) == "\xEF\xBB\xBF") i = 3;
        while (i < n) { std::size_t st = i; bool ok; cp_t x = decode_utf8(b, i, &ok); if (ok) append_utf8(o, x); else bad(st); }
        break; }
    case kind::utf16le: case kind::utf16be: {
        bool le = c->k == kind::utf16le; std::u16string w;
        std::size_t i = 0; if (n >= 2 && ((le && u(0) == 0xFF && u(1) == 0xFE) || (!le && u(0) == 0xFE && u(1) == 0xFF))) i = 2;
        for (; i + 1 < n; i += 2) w += char16_t(le ? (u(i) | u(i + 1) << 8) : (u(i) << 8 | u(i + 1)));
        for (std::size_t k = 0; k < w.size(); ++k) {
            cp_t x = w[k];
            if (x >= 0xD800 && x <= 0xDBFF && k + 1 < w.size() && w[k + 1] >= 0xDC00 && w[k + 1] <= 0xDFFF) { x = 0x10000 + ((x - 0xD800) << 10) + (w[k + 1] - 0xDC00); ++k; }
            else if (x >= 0xD800 && x <= 0xDFFF) { bad(k * 2); continue; }
            append_utf8(o, x);
        }
        if (n % 2) bad(n - 1);
        break; }
    case kind::utf32le: case kind::utf32be: {
        bool le = c->k == kind::utf32le; std::size_t i = 0;
        if (n >= 4 && ((le && b.substr(0, 4) == std::string_view("\xFF\xFE\0\0", 4)) || (!le && b.substr(0, 4) == std::string_view("\0\0\xFE\xFF", 4)))) i = 4;
        for (; i + 3 < n; i += 4) {
            cp_t x = le ? (u(i) | u(i + 1) << 8 | u(i + 2) << 16 | cp_t(u(i + 3)) << 24) : (cp_t(u(i)) << 24 | u(i + 1) << 16 | u(i + 2) << 8 | u(i + 3));
            if (x > 0x10FFFF || (x >= 0xD800 && x <= 0xDFFF)) bad(i); else append_utf8(o, x);
        }
        if (n % 4) bad(n - n % 4);
        break; }
    case kind::ascii: for (std::size_t i = 0; i < n; ++i) if (u(i) < 0x80) o += b[i]; else bad(i); break;
    case kind::latin1: for (std::size_t i = 0; i < n; ++i) append_utf8(o, u(i)); break;
    case kind::single:
        for (std::size_t i = 0; i < n; ++i) {
            if (u(i) < 0x80) { o += b[i]; continue; }
            cp_t x = encdata::single_byte[c->table * 128 + (u(i) - 0x80)];
            if (x) append_utf8(o, x); else bad(i);
        }
        break;
    case kind::sjis: {
        unsigned lead = 0;
        for (std::size_t i = 0; i < n; ++i) {
            unsigned x = u(i);
            if (lead) {
                unsigned off = x < 0x7F ? 0x40 : 0x41, loff = lead < 0xA0 ? 0x81 : 0xC1; long ptr = -1;
                if ((x >= 0x40 && x <= 0x7E) || (x >= 0x80 && x <= 0xFC)) ptr = long((lead - loff) * 188 + x - off);
                lead = 0; cp_t cp = 0;
                if (ptr >= 8836 && ptr <= 10715) cp = cp_t(0xE000 - 8836 + ptr);
                else if (ptr >= 0 && std::size_t(ptr) < std::size(encdata::jis0208)) cp = encdata::jis0208[ptr];
                if (cp) { append_utf8(o, cp); continue; }
                bad(i - 1); if (x < 0x80) --i;
                continue;
            }
            if (x <= 0x80) append_utf8(o, x);
            else if (x >= 0xA1 && x <= 0xDF) append_utf8(o, 0xFF61 - 0xA1 + x);
            else if ((x >= 0x81 && x <= 0x9F) || (x >= 0xE0 && x <= 0xFC)) lead = x;
            else bad(i);
        }
        if (lead) bad(n - 1);
        break; }
    case kind::eucjp: {
        unsigned lead = 0; bool j212 = false;
        for (std::size_t i = 0; i < n; ++i) {
            unsigned x = u(i);
            if (lead == 0x8E && x >= 0xA1 && x <= 0xDF) { lead = 0; append_utf8(o, 0xFF61 - 0xA1 + x); continue; }
            if (lead == 0x8F && x >= 0xA1 && x <= 0xFE) { j212 = true; lead = x; continue; }
            if (lead) {
                cp_t cp = 0;
                if (lead >= 0xA1 && lead <= 0xFE && x >= 0xA1 && x <= 0xFE) {
                    unsigned ptr = (lead - 0xA1) * 94 + x - 0xA1;
                    if (j212) { if (ptr < std::size(encdata::jis0212)) cp = encdata::jis0212[ptr]; }
                    else if (ptr < std::size(encdata::jis0208)) cp = encdata::jis0208[ptr];
                }
                lead = 0; j212 = false;
                if (cp) { append_utf8(o, cp); continue; }
                bad(i - 1); if (x < 0x80) --i;
                continue;
            }
            if (x < 0x80) o += char(x);
            else if (x == 0x8E || x == 0x8F || (x >= 0xA1 && x <= 0xFE)) lead = x;
            else bad(i);
        }
        if (lead) bad(n - 1);
        break; }
    case kind::gbk: case kind::gb18030: {
        unsigned first = 0, second = 0, third = 0;
        for (std::size_t i = 0; i < n; ++i) {
            unsigned x = u(i);
            if (third) {
                cp_t cp = 0;
                if (x >= 0x30 && x <= 0x39) cp = gb_range_cp((((first - 0x81) * 10 + second - 0x30) * 126 + third - 0x81) * 10 + x - 0x30);
                first = second = third = 0;
                if (cp) append_utf8(o, cp); else { bad(i - 3); i -= 3; }   // reprocess second, third, x
                continue;
            }
            if (second) { if (x >= 0x81 && x <= 0xFE) { third = x; continue; } bad(i - 2); i -= 2; first = second = 0; continue; }
            if (first) {
                if (x >= 0x30 && x <= 0x39) { second = x; continue; }
                unsigned lead = first; first = 0; long ptr = -1; unsigned off = x < 0x7F ? 0x40 : 0x41;
                if ((x >= 0x40 && x <= 0x7E) || (x >= 0x80 && x <= 0xFE)) ptr = long((lead - 0x81) * 190 + (x - off));
                cp_t cp = (ptr >= 0 && std::size_t(ptr) < std::size(encdata::gb18030)) ? cp_t(encdata::gb18030[ptr]) : 0;
                if (cp) { append_utf8(o, cp); continue; }
                bad(i - 1); if (x < 0x80) --i;
                continue;
            }
            if (x < 0x80) o += char(x);
            else if (x == 0x80) append_utf8(o, 0x20AC);
            else if (x <= 0xFE) first = x;
            else bad(i);
        }
        if (first || second || third) bad(n - 1);
        break; }
    case kind::big5: {
        unsigned lead = 0;
        for (std::size_t i = 0; i < n; ++i) {
            unsigned x = u(i);
            if (lead) {
                long ptr = -1; unsigned off = x < 0x7F ? 0x40 : 0x62;
                if ((x >= 0x40 && x <= 0x7E) || (x >= 0xA1 && x <= 0xFE)) ptr = long((lead - 0x81) * 157 + (x - off));
                lead = 0;
                if (ptr == 1133) { o += "\u00CA\u0304"; continue; }
                if (ptr == 1135) { o += "\u00CA\u030C"; continue; }
                if (ptr == 1164) { o += "\u00EA\u0304"; continue; }
                if (ptr == 1166) { o += "\u00EA\u030C"; continue; }
                cp_t cp = (ptr >= 0 && std::size_t(ptr) < std::size(encdata::big5)) ? cp_t(encdata::big5[ptr]) : 0;
                if (cp) { append_utf8(o, cp); continue; }
                bad(i - 1); if (x < 0x80) --i;
                continue;
            }
            if (x < 0x80) o += char(x);
            else if (x >= 0x81 && x <= 0xFE) lead = x;
            else bad(i);
        }
        if (lead) bad(n - 1);
        break; }
    case kind::euckr: {
        unsigned lead = 0;
        for (std::size_t i = 0; i < n; ++i) {
            unsigned x = u(i);
            if (lead) {
                long ptr = -1; if (x >= 0x41 && x <= 0xFE) ptr = long((lead - 0x81) * 190 + (x - 0x41));
                lead = 0;
                cp_t cp = (ptr >= 0 && std::size_t(ptr) < std::size(encdata::euc_kr)) ? cp_t(encdata::euc_kr[ptr]) : 0;
                if (cp) { append_utf8(o, cp); continue; }
                bad(i - 1); if (x < 0x80) --i;
                continue;
            }
            if (x < 0x80) o += char(x);
            else if (x >= 0x81 && x <= 0xFE) lead = x;
            else bad(i);
        }
        if (lead) bad(n - 1);
        break; }
    case kind::iso2022jp: {
        enum { st_ascii, st_roman, st_kana, st_jis } st = st_ascii;
        for (std::size_t i = 0; i < n; ++i) {
            unsigned x = u(i);
            if (x == 0x1B) {
                auto seq = b.substr(i + 1, 2);
                if (seq == "(B") st = st_ascii; else if (seq == "(J") st = st_roman; else if (seq == "(I") st = st_kana;
                else if (seq == "$@" || seq == "$B") st = st_jis;
                else { bad(i); continue; }
                i += 2; continue;
            }
            if (x == 0x0E || x == 0x0F || x >= 0x80) { bad(i); continue; }
            if (x == 0x0A || x == 0x0D) { if (st == st_jis) st = st_ascii; o += char(x); continue; }
            switch (st) {
            case st_ascii: o += char(x); break;
            case st_roman: append_utf8(o, x == 0x5C ? 0xA5 : x == 0x7E ? 0x203E : x); break;
            case st_kana: if (x >= 0x21 && x <= 0x5F) append_utf8(o, 0xFF61 - 0x21 + x); else bad(i); break;
            case st_jis: {
                if (i + 1 >= n || x < 0x21 || x > 0x7E || u(i + 1) < 0x21 || u(i + 1) > 0x7E) { bad(i); break; }
                unsigned ptr = (x - 0x21) * 94 + (u(i + 1) - 0x21); cp_t cp = ptr < std::size(encdata::jis0208) ? cp_t(encdata::jis0208[ptr]) : 0;
                if (cp) append_utf8(o, cp); else bad(i);
                ++i; break; }
            }
        }
        break; }
    }
    if (failed) { r.value.clear(); r.err = {EILSEQ, "decode(" + std::string(c->name) + "): invalid byte sequence at offset " + std::to_string(fail_at)}; }
    return r;
}

result<std::string> encode(std::string_view s, std::string_view charset, on_error onerr) {
    result<std::string> r;
    auto c = lookup(charset);
    if (!c) { r.err = {EINVAL, "encode: unknown charset '" + std::string(charset) + "'"}; return r; }
    std::string& o = r.value; bool failed = false; cp_t fail_cp = 0;
    auto bad = [&](cp_t x) {
        if (onerr == on_error::fail) { if (!failed) { failed = true; fail_cp = x; } }
        else if (onerr == on_error::replace) o += '?';
    };
    auto put16 = [&](unsigned v, bool le) { if (le) { o += char(v & 0xFF); o += char(v >> 8); } else { o += char(v >> 8); o += char(v & 0xFF); } };
    std::size_t i = 0; int jstate = 0;
    const reverse_maps* R = (c->k >= kind::single) ? &rev() : nullptr;
    while (i < s.size()) {
        cp_t x = decode_utf8(s, i);
        switch (c->k) {
        case kind::utf8: append_utf8(o, x); break;
        case kind::utf16le: case kind::utf16be: { std::u16string w; append_utf16(w, x); for (auto u16 : w) put16(u16, c->k == kind::utf16le); break; }
        case kind::utf32le: for (int k = 0; k < 4; ++k) o += char((x >> (8 * k)) & 0xFF); break;
        case kind::utf32be: for (int k = 3; k >= 0; --k) o += char((x >> (8 * k)) & 0xFF); break;
        case kind::ascii: if (x < 0x80) o += char(x); else bad(x); break;
        case kind::latin1: if (x < 0x100) o += char(x); else bad(x); break;
        case kind::single: {
            if (x < 0x80) { o += char(x); break; }
            auto& m = R->sb[c->table]; if (auto it = m.find(x); it != m.end()) o += char(it->second); else bad(x);
            break; }
        case kind::sjis: {
            if (x < 0x80) { o += char(x); break; }
            if (x == 0xA5) { o += '\x5C'; break; } if (x == 0x203E) { o += '\x7E'; break; }
            if (x >= 0xFF61 && x <= 0xFF9F) { o += char(x - 0xFF61 + 0xA1); break; }
            if (x == 0x2212) x = 0xFF0D;
            auto it = R->sjis.find(x); if (it == R->sjis.end()) { bad(x); break; }
            unsigned p = it->second, lead = p / 188, loff = lead < 0x1F ? 0x81 : 0xC1, tr = p % 188, off = tr < 0x3F ? 0x40 : 0x41;
            o += char(lead + loff); o += char(tr + off); break; }
        case kind::eucjp: {
            if (x < 0x80) { o += char(x); break; }
            if (x == 0xA5) { o += '\x5C'; break; } if (x == 0x203E) { o += '\x7E'; break; }
            if (x >= 0xFF61 && x <= 0xFF9F) { o += '\x8E'; o += char(x - 0xFF61 + 0xA1); break; }
            if (x == 0x2212) x = 0xFF0D;
            auto it = R->jis.find(x); if (it == R->jis.end()) { bad(x); break; }
            o += char(it->second / 94 + 0xA1); o += char(it->second % 94 + 0xA1); break; }
        case kind::gbk: case kind::gb18030: {
            if (x < 0x80) { o += char(x); break; }
            if (x == 0xE5E5) { bad(x); break; }
            if (c->k == kind::gbk && x == 0x20AC) { o += '\x80'; break; }
            if (auto it = R->gb.find(x); it != R->gb.end()) {
                unsigned p = it->second, tr = p % 190; o += char(p / 190 + 0x81); o += char(tr + (tr < 0x3F ? 0x40 : 0x41)); break;
            }
            if (c->k == kind::gbk) { bad(x); break; }
            unsigned p = gb_range_ptr(x);
            unsigned b1 = p / (10 * 126 * 10); p %= 10 * 126 * 10; unsigned b2 = p / (10 * 126); p %= 10 * 126;
            o += char(b1 + 0x81); o += char(b2 + 0x30); o += char(p / 10 + 0x81); o += char(p % 10 + 0x30); break; }
        case kind::big5: {
            if (x < 0x80) { o += char(x); break; }
            auto it = R->big5.find(x); if (it == R->big5.end()) { bad(x); break; }
            unsigned p = it->second, tr = p % 157; o += char(p / 157 + 0x81); o += char(tr + (tr < 0x3F ? 0x40 : 0x62)); break; }
        case kind::euckr: {
            if (x < 0x80) { o += char(x); break; }
            auto it = R->kr.find(x); if (it == R->kr.end()) { bad(x); break; }
            o += char(it->second / 190 + 0x81); o += char(it->second % 190 + 0x41); break; }
        case kind::iso2022jp: {
            if (x < 0x80) { if (jstate != 0) { o += "\x1B(B"; jstate = 0; } if (x == 0x0E || x == 0x0F || x == 0x1B) bad(x); else o += char(x); break; }
            if (x == 0x2212) x = 0xFF0D;
            if (x >= 0xFF61 && x <= 0xFF9F) { if (jstate != 2) { o += "\x1B(I"; jstate = 2; } o += char(x - 0xFF61 + 0x21); break; }
            auto it = R->jis.find(x); if (it == R->jis.end()) { bad(x); break; }
            if (jstate != 1) { o += "\x1B$B"; jstate = 1; }
            o += char(it->second / 94 + 0x21); o += char(it->second % 94 + 0x21); break; }
        }
    }
    if (c->k == kind::iso2022jp && jstate != 0) o += "\x1B(B";
    if (failed) {
        char hex[16]; std::snprintf(hex, sizeof hex, "U+%04X", unsigned(fail_cp));
        r.value.clear(); r.err = {EILSEQ, "encode(" + std::string(c->name) + "): " + hex + " is not representable"};
    }
    return r;
}

bom_info detect_bom(std::string_view b) {
    if (b.size() >= 4 && b.substr(0, 4) == std::string_view("\xFF\xFE\0\0", 4)) return {"utf-32le", 4};
    if (b.size() >= 4 && b.substr(0, 4) == std::string_view("\0\0\xFE\xFF", 4)) return {"utf-32be", 4};
    if (b.size() >= 3 && b.substr(0, 3) == "\xEF\xBB\xBF") return {"utf-8", 3};
    if (b.size() >= 2 && b.substr(0, 2) == "\xFF\xFE") return {"utf-16le", 2};
    if (b.size() >= 2 && b.substr(0, 2) == "\xFE\xFF") return {"utf-16be", 2};
    return {};
}

charset_guess sniff_charset(std::string_view b, std::string_view hint) {
    if (auto bom = detect_bom(b); !bom.charset.empty()) return {bom.charset, 1.0};
    bool ascii = true; for (unsigned char c : b) if (c >= 0x80) { ascii = false; break; }
    // UTF-16 without BOM: lots of NULs on one parity.
    if (b.size() >= 4) {
        std::size_t ev = 0, od = 0; for (std::size_t i = 0; i + 1 < b.size(); i += 2) { ev += b[i] == 0; od += b[i + 1] == 0; }
        double half = double(b.size() / 2);
        if (od > half * 0.3 && ev < half * 0.05) return {"utf-16le", 0.8};
        if (ev > half * 0.3 && od < half * 0.05) return {"utf-16be", 0.8};
    }
    if (ascii && (b.find("\x1B$B") != std::string_view::npos || b.find("\x1B$@") != std::string_view::npos)) return {"iso-2022-jp", 0.95};
    if (ascii) return {"utf-8", 1.0};
    if (is_valid_utf8(b)) return {"utf-8", 0.99};
    auto lang = lower(hint).substr(0, 2);
    struct cand { const char* cs; const char* lang; };
    static const cand cands[] = {{"shift_jis", "ja"}, {"euc-jp", "ja"}, {"gbk", "zh"}, {"big5", "zh"}, {"euc-kr", "ko"},
        {"windows-1252", "en"}, {"windows-1250", "pl"}, {"windows-1251", "ru"}, {"koi8-r", "ru"}, {"windows-1253", "el"},
        {"windows-1254", "tr"}, {"windows-1255", "he"}, {"windows-1256", "ar"}, {"windows-874", "th"}};
    double gbfrac = 1.0;
    {   std::size_t pairs = 0, euc = 0;
        for (std::size_t k = 0; k + 1 < b.size(); ++k) if ((unsigned char)b[k] >= 0x81) { ++pairs; if ((unsigned char)b[k] >= 0xA1 && (unsigned char)b[k + 1] >= 0xA1) ++euc; ++k; }
        if (pairs) gbfrac = double(euc) / double(pairs); }
    charset_guess best{"windows-1252", 0.1}; double bestscore = -1e9;
    for (auto& cd : cands) {
        auto r = decode(b, cd.cs, on_error::fail);
        if (!r) continue;
        // Score: share of non-ASCII code points that are letters of one coherent script, lowercase bonus for alphabets.
        std::size_t i = 0, nonascii = 0, good = 0, lowerc = 0, cjk = 0, kana = 0, latin_adj = 0, common = 0; bool prev_latin = false; std::unordered_map<std::string_view, int> scripts;
        while (i < r.value.size()) {
            cp_t c = decode_utf8(r.value, i); if (c < 0x80) { prev_latin = false; continue; }
            {   static const std::u32string cyr = U"оеаинтсрвлкмдпуяыь", grk = U"αοιετσνηυρκπμλάέίόςή";
                cp_t lc = simple_lower(c);
                if (cyr.find(lc) != std::u32string::npos || grk.find(lc) != std::u32string::npos) ++common; }
            bool lat = script(c) == "Latin"; if (lat && prev_latin) ++latin_adj; prev_latin = lat;
            ++nonascii;
            if (is_alpha(c)) { ++good; ++scripts[script(c)]; if (is_lower(c)) ++lowerc; }
            if (script(c) == "Hiragana" || script(c) == "Katakana") ++kana;
            if (is_ideographic(c) || script(c) == "Hiragana" || script(c) == "Katakana" || script(c) == "Hangul") ++cjk;
        }
        if (!nonascii) continue;
        int top = 0; for (auto& [k, v] : scripts) top = std::max(top, v);
        double score = double(top) / double(nonascii) + 0.3 * double(lowerc) / double(nonascii);
        score += 0.6 * double(common) / double(nonascii);   // letter-frequency hint for Cyrillic vs Greek
        score -= 0.8 * double(latin_adj) / double(nonascii);   // runs of accented Latin letters are rare in real text
        bool multibyte = std::string_view(cd.cs) == "shift_jis" || std::string_view(cd.cs) == "euc-jp" || std::string_view(cd.cs) == "gbk" ||
                         std::string_view(cd.cs) == "big5" || std::string_view(cd.cs) == "euc-kr";
        if (multibyte) {
            score = double(cjk) / double(nonascii) + (nonascii * 2 <= b.size() ? 0.35 : 0.0);
            std::string_view csn = cd.cs;
            if (csn == "shift_jis" || csn == "euc-jp") score += kana ? 0.5 * double(kana) / double(nonascii) : -0.3;
            if (csn == "gbk" || csn == "euc-kr" || csn == "euc-jp") score *= gbfrac;   // EUC-style text keeps both bytes in A1..FE
        }
        if (!lang.empty() && lang == cd.lang) score += 0.4;
        if (std::string_view(cd.cs) == "windows-1252") score += 0.05;   // tie-break toward the most common legacy charset
        if (score > bestscore) { bestscore = score; best = {cd.cs, std::min(0.95, std::max(0.1, score / 1.7))}; }
    }
    return best;
}
std::string to_utf8_auto(std::string_view b, std::string* used) {
    auto g = sniff_charset(b);
    auto r = decode(b, g.charset, on_error::replace);
    if (!r) { g.charset = "windows-1252"; r = decode(b, g.charset, on_error::replace); }
    if (used) *used = g.charset;
    return r.value;
}
}  // namespace forge::os::text
