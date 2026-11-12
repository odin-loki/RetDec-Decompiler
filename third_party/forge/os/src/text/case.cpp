// forge::os::text: full case mapping and folding (UnicodeData + SpecialCasing + CaseFolding).
#include "internal.hpp"

namespace forge::os::text {
using namespace detail;
namespace {
std::string lang_of(std::string_view tag) {
    std::string l; for (char c : tag) { if (c == '-' || c == '_') break; l += char(c | 0x20); } return l;
}
template <class F> void each_cp(std::string_view s, F&& f) { std::size_t i = 0; while (i < s.size()) f(decode_utf8(s, i)); }
void put(std::string& o, std::span<const unsigned> m) { for (auto c : m) append_utf8(o, cp_t(c)); }
bool final_sigma(const std::u32string& v, std::size_t i) {
    // C is preceded by a cased letter (skipping case-ignorables) and not followed by one.
    std::size_t j = i; bool before = false;
    while (j > 0) { --j; if (is_case_ignorable(v[j])) continue; before = is_cased(v[j]); break; }
    if (!before) return false;
    for (std::size_t k = i + 1; k < v.size(); ++k) { if (is_case_ignorable(v[k])) continue; return !is_cased(v[k]); }
    return true;
}
bool more_above(const std::u32string& v, std::size_t i) {
    for (std::size_t k = i + 1; k < v.size(); ++k) { int cc = combining_class(v[k]); if (cc == 0) return false; if (cc == 230) return true; }
    return false;
}
bool after_soft_dotted(const std::u32string& v, std::size_t i) {
    for (std::size_t k = i; k > 0; --k) {
        cp_t c = v[k - 1]; int cc = combining_class(c);
        if (c == 'i' || c == 'j' || c == 0x012F || c == 0x0268 || c == 0x0456 || c == 0x0458 || c == 0x1E2D || c == 0x1ECB) return true;
        if (cc == 0 || cc == 230) return false;
    }
    return false;
}
bool after_I(const std::u32string& v, std::size_t i) {
    for (std::size_t k = i; k > 0; --k) { cp_t c = v[k - 1]; if (c == 'I') return true; int cc = combining_class(c); if (cc == 0 || cc == 230) return false; }
    return false;
}
void lower_one(std::string& o, const std::u32string& v, std::size_t i, const std::string& lang) {
    cp_t c = v[i];
    if (lang == "tr" || lang == "az") {
        if (c == 0x0130) { o += 'i'; return; }
        if (c == 0x0307 && after_I(v, i)) return;
        if (c == 'I') { bool dot = false; for (std::size_t k = i + 1; k < v.size(); ++k) { if (v[k] == 0x0307) { dot = true; break; } int cc = combining_class(v[k]); if (cc == 0 || cc == 230) break; }
            append_utf8(o, dot ? cp_t('i') : cp_t(0x0131)); return; }
    }
    if (lang == "lt") {
        if ((c == 'I' || c == 'J' || c == 0x012E) && more_above(v, i)) { append_utf8(o, simple_lower(c)); append_utf8(o, 0x0307); return; }
        if (c == 0x00CC) { o += "i\xCC\x87\xCC\x80"; return; }
        if (c == 0x00CD) { o += "i\xCC\x87\xCC\x81"; return; }
        if (c == 0x0128) { o += "i\xCC\x87\xCC\x83"; return; }
    }
    if (c == 0x03A3) { append_utf8(o, final_sigma(v, i) ? cp_t(0x03C2) : cp_t(0x03C3)); return; }
    if (auto m = special_lower(c); !m.empty()) { put(o, m); return; }
    append_utf8(o, simple_lower(c));
}
void upper_one(std::string& o, const std::u32string& v, std::size_t i, const std::string& lang, bool title) {
    cp_t c = v[i];
    if ((lang == "tr" || lang == "az") && c == 'i') { append_utf8(o, 0x0130); return; }
    if (lang == "lt" && c == 0x0307 && after_soft_dotted(v, i)) return;
    if (auto m = title ? special_title(c) : special_upper(c); !m.empty()) { put(o, m); return; }
    append_utf8(o, title ? simple_title(c) : simple_upper(c));
}
}  // namespace

std::string to_lower(std::string_view s, std::string_view tag) {
    auto v = to32(s); auto lang = lang_of(tag); std::string o; o.reserve(s.size());
    for (std::size_t i = 0; i < v.size(); ++i) lower_one(o, v, i, lang);
    return o;
}
std::string to_upper(std::string_view s, std::string_view tag) {
    auto v = to32(s); auto lang = lang_of(tag); std::string o; o.reserve(s.size());
    for (std::size_t i = 0; i < v.size(); ++i) upper_one(o, v, i, lang, false);
    return o;
}
std::string to_title(std::string_view s, std::string_view tag) {
    auto v = to32(s); auto lang = lang_of(tag); std::string o; o.reserve(s.size());
    auto wb = word_breaks32(v); std::size_t w = 0;
    bool seen_cased = false;
    for (std::size_t i = 0; i < v.size(); ++i) {
        if (w < wb.size() && wb[w] == i) { seen_cased = false; ++w; }
        if (!seen_cased && is_cased(v[i])) {
            seen_cased = true;
            if (lang == "nl" && (v[i] == 'i' || v[i] == 'I') && i + 1 < v.size() && (v[i + 1] == 'j' || v[i + 1] == 'J')) {
                o += "IJ"; ++i; continue;
            }
            upper_one(o, v, i, lang, true);
        } else if (seen_cased) lower_one(o, v, i, lang);
        else append_utf8(o, v[i]);
    }
    return o;
}
std::string case_fold(std::string_view s, bool turkic) {
    std::string o; o.reserve(s.size());
    each_cp(s, [&](cp_t c) {
        if (turkic && c == 'I') { append_utf8(o, 0x0131); return; }
        if (turkic && c == 0x0130) { o += 'i'; return; }
        if (auto m = full_fold(c); !m.empty()) put(o, m); else append_utf8(o, simple_fold(c));
    });
    return o;
}
bool equals_ignore_case(std::string_view a, std::string_view b) {
    return normalize(case_fold(normalize(a, norm_form::nfd)), norm_form::nfd) == normalize(case_fold(normalize(b, norm_form::nfd)), norm_form::nfd);
}
}  // namespace forge::os::text
