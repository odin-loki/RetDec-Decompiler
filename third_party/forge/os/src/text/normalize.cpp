// forge::os::text: Unicode normalization forms (UAX 15).
#include "internal.hpp"
#include <algorithm>

namespace forge::os::text {
using namespace detail;
namespace {
constexpr cp_t SBase = 0xAC00, LBase = 0x1100, VBase = 0x1161, TBase = 0x11A7;
constexpr int LCount = 19, VCount = 21, TCount = 28, NCount = VCount * TCount, SCount = LCount * NCount;
void decompose(cp_t c, bool compat, std::u32string& o) {
    if (c >= SBase && c < SBase + SCount) {
        int s = int(c - SBase);
        o += cp_t(LBase + s / NCount); o += cp_t(VBase + (s % NCount) / TCount);
        if (s % TCount) o += cp_t(TBase + s % TCount);
        return;
    }
    bool is_compat = false; auto d = decomposition(c, is_compat);
    if (d.empty() || (is_compat && !compat)) { o += c; return; }
    for (auto x : d) decompose(cp_t(x), compat, o);
}
void reorder(std::u32string& v) {
    // Canonical ordering: stable sort each run of non-starters by combining class.
    std::size_t i = 0;
    while (i < v.size()) {
        if (combining_class(v[i]) == 0) { ++i; continue; }
        std::size_t j = i; while (j < v.size() && combining_class(v[j]) != 0) ++j;
        if (j - i > 1) std::stable_sort(v.begin() + i, v.begin() + j, [](cp_t a, cp_t b) { return combining_class(a) < combining_class(b); });
        i = j;
    }
}
cp_t compose_pair(cp_t a, cp_t b) {
    if (a >= LBase && a < LBase + LCount && b >= VBase && b < VBase + VCount) return SBase + ((a - LBase) * VCount + (b - VBase)) * TCount;
    if (a >= SBase && a < SBase + SCount && (a - SBase) % TCount == 0 && b > TBase && b < TBase + TCount) return a + (b - TBase);
    return compose(a, b);
}
void compose_all(std::u32string& v) {
    std::size_t starter = std::u32string::npos, out = 0; int last_cc = 0;
    for (std::size_t i = 0; i < v.size(); ++i) {
        cp_t c = v[i]; int cc = combining_class(c);
        if (starter != std::u32string::npos) {
            bool blocked = out != starter + 1 && (last_cc == 0 || last_cc >= cc);
            if (!blocked) if (cp_t k = compose_pair(v[starter], c)) { v[starter] = k; continue; }
        }
        if (cc == 0) starter = out;
        last_cc = cc; v[out++] = c;
    }
    v.resize(out);
}
}  // namespace

std::u32string normalize32(std::u32string_view s, norm_form f) {
    bool compat = f == norm_form::nfkc || f == norm_form::nfkd;
    std::u32string v; v.reserve(s.size() + s.size() / 4);
    for (auto c : s) decompose(c, compat, v);
    reorder(v);
    if (f == norm_form::nfc || f == norm_form::nfkc) compose_all(v);
    return v;
}
std::string normalize(std::string_view s, norm_form f) {
    bool ascii = true; for (unsigned char c : s) if (c >= 0x80) { ascii = false; break; }
    if (ascii) return std::string(s);   // ASCII is invariant under every form
    return encode_utf8(normalize32(to32(s), f));
}
bool is_normalized(std::string_view s, norm_form f) { return normalize(s, f) == s; }
}  // namespace forge::os::text
