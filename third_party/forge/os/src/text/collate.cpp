// forge::os::text: Unicode Collation Algorithm (UTS 10) over the DUCET with simple locale tailorings.
// Weights are widened internally (primary << 8, secondary << 4, tertiary << 2) so tailoring rules can
// insert characters between DUCET weights without renumbering the table.
#include "internal.hpp"
#include "../../../core/include/forge/core/memory.hpp"
#include <algorithm>
#include <mutex>
#include <unordered_map>

namespace forge::os::text {
using namespace detail;
namespace {
struct ce { std::uint32_t p; std::uint16_t s; std::uint8_t t; bool var; };
using ce_list = std::vector<ce>;
struct table {
    std::unordered_map<std::u32string, ce_list> map;
    std::size_t max_key = 1;
    std::uint32_t digit0 = 0;            // primary of '0' (numeric collation)
};
ce widen(unsigned x) { return ce{(x >> 15) << 8, std::uint16_t(((x >> 6) & 0x1FF) << 4), std::uint8_t(((x >> 1) & 0x1F) << 2), bool(x & 1)}; }
const table& base_table() {
    static table T; static std::once_flag once;
    std::call_once(once, [] {
        auto& d = ducet();
        T.map.reserve(d.key_o.size() * 2);
        for (std::size_t i = 0; i < d.key_o.size(); ++i) {
            unsigned ko = d.key_o[i], co = d.ce_o[i];
            std::u32string k; for (unsigned j = 0; j < (ko & 3); ++j) k += cp_t(d.key_pool[(ko >> 2) + j]);
            ce_list l; for (unsigned j = 0; j < (co & 31); ++j) l.push_back(widen(d.ce_pool[(co >> 5) + j]));
            T.max_key = std::max(T.max_key, k.size());
            T.map.emplace(std::move(k), std::move(l));
        }
        T.digit0 = T.map[U"0"][0].p;
    });
    return T;
}
bool unified_ideo(cp_t c) { return prop(c).flags & F_UIdeo; }
void implicit(cp_t c, ce_list& out) {
    for (auto& r : ducet().implicits) if (c >= r.lo && c <= r.hi) {
        unsigned first = r.lo;   // BBBB counts from the first range sharing this base (Tangut: 17000)
        for (auto& q : ducet().implicits) if (q.base == r.base) first = std::min(first, q.lo);
        out.push_back({r.base << 8, 0x20 << 4, 0x02 << 2, false}); out.push_back({((c - first) | 0x8000) << 8, 0, 0, false}); return;
    }
    unsigned base = 0xFBC0;
    if (unified_ideo(c)) base = ((c >= 0x4E00 && c <= 0x9FFF) || (c >= 0xF900 && c <= 0xFAFF)) ? 0xFB40 : 0xFB80;
    out.push_back({(base + (c >> 15)) << 8, 0x20 << 4, 0x02 << 2, false});
    out.push_back({((c & 0x7FFF) | 0x8000) << 8, 0, 0, false});
}

// Tailoring rules: "&anchor < x <<< X << y = z" (subset of CLDR/ICU syntax).
const char* tailoring_for(std::string_view loc, bool& backwards) {
    std::string l; for (char c : loc) l += char(c == '_' ? '-' : (c | 0x20));
    backwards = l == "fr-ca";
    auto lang = l.substr(0, l.find('-'));
    if (l.find("phonebk") != std::string::npos && lang == "de") return "&AE<<\u00E4<<<\u00C4&OE<<\u00F6<<<\u00D6&UE<<\u00FC<<<\u00DC";
    if (lang == "sv") return "&D<<\u0111<<<\u0110<<\u00F0<<<\u00D0&t<<<\u00FE&y<<\u00FC<<<\u00DC&z<\u00E5<<<\u00C5<\u00E4<<<\u00C4<<\u00E6<<<\u00C6<<\u0119<<<\u0118<\u00F6<<<\u00D6<<\u00F8<<<\u00D8<<\u0151<<<\u0150<<\u0153<<<\u0152";
    if (lang == "fi") return "&D<<\u0111<<<\u0110&N<<\u014B<<<\u014A&T<<\u0167<<<\u0166&Y<<\u00FC<<<\u00DC&Z<<\u017E<<<\u017D&z<\u00E5<<<\u00C5<\u00E4<<<\u00C4<<\u00E6<<<\u00C6<\u00F6<<<\u00D6<<\u00F8<<<\u00D8";
    if (lang == "da" || lang == "nb" || lang == "nn" || lang == "no") return "&D<<\u0111<<<\u0110<<\u00F0<<<\u00D0&t<<<\u00FE&y<<\u00FC<<<\u00DC&z<\u00E6<<<\u00C6<<\u00E4<<<\u00C4<\u00F8<<<\u00D8<<\u00F6<<<\u00D6<\u00E5<<<\u00C5";
    if (lang == "es") return "&N<\u00F1<<<\u00D1";
    if (lang == "tr" || lang == "az") return "&C<\u00E7<<<\u00C7&G<\u011F<<<\u011E&H<\u0131<<<I&i<<<\u0130&O<\u00F6<<<\u00D6&S<\u015F<<<\u015E&U<\u00FC<<<\u00DC";
    if (lang == "pl") return "&A<\u0105<<<\u0104&C<\u0107<<<\u0106&E<\u0119<<<\u0118&L<\u0142<<<\u0141&N<\u0144<<<\u0143&O<\u00F3<<<\u00D3&S<\u015B<<<\u015A&Z<\u017A<<<\u0179<\u017C<<<\u017B";
    if (lang == "cs") return "&C<\u010D<<<\u010C&H<ch<<<cH<<<Ch<<<CH&R<\u0159<<<\u0158&S<\u0161<<<\u0160&Z<\u017E<<<\u017D";
    if (lang == "sk") return "&A<\u00E4<<<\u00C4&C<\u010D<<<\u010C&H<ch<<<cH<<<Ch<<<CH&O<\u00F4<<<\u00D4&R<\u0159<<<\u0158&S<\u0161<<<\u0160&Z<\u017E<<<\u017D";
    if (lang == "hr" || lang == "bs") return "&C<\u010D<<<\u010C<\u0107<<<\u0106&D<d\u017E<<<D\u017E<<<D\u017D<\u0111<<<\u0110&L<lj<<<Lj<<<LJ&N<nj<<<Nj<<<NJ&S<\u0161<<<\u0160&Z<\u017E<<<\u017D";
    if (lang == "et") return "&S<\u0161<<<\u0160<z<<<Z<\u017E<<<\u017D&W<\u00F5<<<\u00D5<\u00E4<<<\u00C4<\u00F6<<<\u00D6<\u00FC<<<\u00DC";
    if (lang == "lt") return "&C<\u010D<<<\u010C&I<y<<<Y&S<\u0161<<<\u0160&Z<\u017E<<<\u017D";
    if (lang == "lv") return "&C<\u010D<<<\u010C&G<\u0123<<<\u0122&I<y<<<Y&K<\u0137<<<\u0136&L<\u013C<<<\u013B&N<\u0146<<<\u0145&S<\u0161<<<\u0160&Z<\u017E<<<\u017D";
    if (lang == "hu") return "&C<cs<<<Cs<<<CS&D<dz<<<Dz<<<DZ<dzs<<<Dzs<<<DZS&G<gy<<<Gy<<<GY&L<ly<<<Ly<<<LY&N<ny<<<Ny<<<NY&O<\u00F6<<<\u00D6<<\u0151<<<\u0150&S<sz<<<Sz<<<SZ&T<ty<<<Ty<<<TY&U<\u00FC<<<\u00DC<<\u0171<<<\u0170&Z<zs<<<Zs<<<ZS";
    if (lang == "is") return "&D<\u00F0<<<\u00D0&Z<\u00FE<<<\u00DE<\u00E6<<<\u00C6<\u00F6<<<\u00D6";
    if (lang == "ro") return "&A<\u0103<<<\u0102<\u00E2<<<\u00C2&I<\u00EE<<<\u00CE&S<\u0219<<<\u0218<<\u015F<<<\u015E&T<\u021B<<<\u021A<<\u0163<<<\u0162";
    if (lang == "cy") return "&C<ch<<<Ch<<<CH&D<dd<<<Dd<<<DD&F<ff<<<Ff<<<FF&G<ng<<<Ng<<<NG&L<ll<<<Ll<<<LL&P<ph<<<Ph<<<PH&R<rh<<<Rh<<<RH&T<th<<<Th<<<TH";
    return nullptr;
}
}  // namespace

struct collator::impl {
    collation_options o;
    std::unordered_map<std::u32string, ce_list> tail;   // overrides on top of the DUCET
    std::size_t max_key = 3;
    mem::lease lease;
    bool lookup(const std::u32string& k, ce_list& out) const {
        if (auto it = tail.find(k); it != tail.end()) { out = it->second; return true; }
        auto& b = base_table().map; if (auto it = b.find(k); it != b.end()) { out = it->second; return true; }
        return false;
    }
    bool has(const std::u32string& k) const { return tail.count(k) || base_table().map.count(k); }
    ce_list ces_for(std::u32string_view str) const {
        std::u32string s = normalize32(str, norm_form::nfd);
        ce_list out; ce_list tmp;
        std::size_t i = 0;
        while (i < s.size()) {
            if (o.numeric && is_digit(s[i])) {
                std::size_t j = i; std::vector<int> d;
                while (j < s.size() && is_digit(s[j])) { d.push_back(*digit_value(s[j])); ++j; }
                std::size_t z = 0; while (z + 1 < d.size() && d[z] == 0) ++z;
                std::size_t len = std::min<std::size_t>(d.size() - z, 99);
                auto D = base_table().digit0;
                out.push_back({D + (std::uint32_t(len / 10) << 8), 0x20 << 4, 0x02 << 2, false});
                out.push_back({D + (std::uint32_t(len % 10) << 8), 0x20 << 4, 0x02 << 2, false});
                for (std::size_t k = z; k < d.size(); ++k) out.push_back({D + (std::uint32_t(d[k]) << 8), 0x20 << 4, 0x02 << 2, false});
                i = j; continue;
            }
            std::size_t len = std::min(max_key, s.size() - i);
            bool found = false;
            for (; len >= 1; --len) if (lookup(s.substr(i, len), tmp)) { found = true; break; }
            if (!found) { tmp.clear(); implicit(s[i], tmp); len = 1; }
            else {
                // Discontiguous contractions (S2.1.1-S2.1.3): extend with unblocked non-starters.
                std::u32string key = s.substr(i, len);
                int max_skipped = -1;
                std::size_t j = i + len;
                while (j < s.size()) {
                    int cc = combining_class(s[j]); if (cc == 0) break;
                    bool blocked = max_skipped >= cc;
                    if (!blocked) {
                        std::u32string k2 = key + s[j];
                        if (ce_list t2; lookup(k2, t2)) {
                            if (j == i + len) { key = k2; tmp = t2; ++len; ++j; continue; }
                            key = k2; tmp = t2; s.erase(j, 1); continue;
                        }
                    }
                    max_skipped = std::max(max_skipped, cc); ++j;
                }
            }
            out.insert(out.end(), tmp.begin(), tmp.end());
            i += len;
        }
        return out;
    }
};

collator::collator(std::string_view locale, collation_options o) : p_(std::make_unique<impl>()) {
    p_->o = o; base_table();
    bool bw = false;
    const char* rules = tailoring_for(locale, bw);
    if (bw) p_->o.backwards_secondary = true;
    if (!rules) return;
    // Parse rules.
    std::string_view r(rules); std::size_t i = 0; ce_list last; int bumps[4] = {0, 0, 0, 0};
    auto read_item = [&]() { std::string it; while (i < r.size() && r[i] != '<' && r[i] != '&' && r[i] != '=') it += r[i++]; return it; };
    while (i < r.size()) {
        if (r[i] == '&') { ++i; auto a = read_item(); last = p_->ces_for(to32(a)); continue; }
        int lvl = 0;
        if (r[i] == '=') { lvl = 0; ++i; }
        else { while (i < r.size() && r[i] == '<') { ++lvl; ++i; } }
        auto item = read_item();
        if (last.empty()) continue;
        ce_list n = last; ce& c = n.back();
        if (lvl == 1) { c.p += 1 + bumps[1]; c.s = 0x20 << 4; c.t = 0x02 << 2; }
        else if (lvl == 2) { c.s += 1; c.t = 0x02 << 2; }
        else if (lvl == 3) { c.t += 1; }
        // Expansion anchors (e.g. &AE<<ä) keep the full expansion; contractions become keys.
        auto key = normalize32(to32(item), norm_form::nfd);
        p_->tail[key] = n; p_->max_key = std::max(p_->max_key, key.size());
        last = n;
    }
    std::size_t bytes = 0; for (auto& [k, v] : p_->tail) bytes += k.size() * 4 + v.size() * sizeof(ce) + 64;
    p_->lease = mem::host_budget().try_reserve(bytes, "text");
}
collator::~collator() = default;
collator::collator(collator&&) noexcept = default;
collator& collator::operator=(collator&&) noexcept = default;
const collation_options& collator::options() const { return p_->o; }

std::string collator::sort_key32(std::u32string_view s) const {
    auto ces = p_->ces_for(s);
    const auto& o = p_->o; int L = int(o.level);
    std::string k;
    auto put16 = [&](unsigned v) { k += char(v >> 8); k += char(v & 0xFF); };
    auto put24 = [&](unsigned v) { k += char(v >> 16); k += char((v >> 8) & 0xFF); k += char(v & 0xFF); };
    // Variable weighting (UTS 10 table 11): shifted CEs move their primary to level 4.
    std::vector<unsigned> q4; bool after_var = false;
    if (o.shifted) {
        for (auto& c : ces) {
            if (c.var && c.p) { q4.push_back(c.p); c.p = 0; c.s = 0; c.t = 0; after_var = true; }
            else if (!c.p && !c.s && !c.t) {}
            else if (!c.p && after_var) { c.s = 0; c.t = 0; }
            else { q4.push_back(0xFFFFFF); if (c.p) after_var = false; }
        }
    }
    for (auto& c : ces) if (c.p) put24(c.p);
    if (L >= 2) {
        put24(0);
        std::vector<unsigned> sec; for (auto& c : ces) if (c.s) sec.push_back(c.s);
        if (o.backwards_secondary) std::reverse(sec.begin(), sec.end());
        for (auto v : sec) put16(v);
    }
    if (L >= 3) {
        put16(0);
        auto upper = [](unsigned t) { t >>= 2; return t == 0x08 || t == 0x09 || t == 0x0A || t == 0x0B || t == 0x0C || t == 0x0E || t == 0x11 || t == 0x12 || t == 0x1D; };
        for (auto& c : ces) if (c.t) k += char(o.upper_first ? ((upper(c.t) ? 0x01 : 0x80) + (c.t >> 1)) : (0x01 + (c.t >> 1)));
    }
    bool l4 = L >= 4 && o.shifted;
    if (l4) { k += char(0); for (auto v : q4) put24(v); }
    if (L >= 5) {
        if (l4) put24(0); else k += char(0);
        for (auto c : normalize32(s, norm_form::nfd)) { k += char(((c >> 16) & 0xFF) + 1); k += char((c >> 8) & 0xFF); k += char(c & 0xFF); }
    }
    return k;
}
std::string collator::sort_key(std::string_view s) const { return sort_key32(to32(s)); }
int collator::compare(std::string_view a, std::string_view b) const {
    auto ka = sort_key(a), kb = sort_key(b);
    int c = ka.compare(kb); return c < 0 ? -1 : c > 0 ? 1 : 0;
}
void sort(std::vector<std::string>& v, std::string_view locale, collation_options o) {
    collator c(locale, o);
    std::vector<std::pair<std::string, std::size_t>> keys; keys.reserve(v.size());
    for (std::size_t i = 0; i < v.size(); ++i) keys.push_back({c.sort_key(v[i]), i});
    std::stable_sort(keys.begin(), keys.end(), [](auto& a, auto& b) { return a.first < b.first; });
    std::vector<std::string> out; out.reserve(v.size()); for (auto& k : keys) out.push_back(std::move(v[k.second]));
    v = std::move(out);
}
}  // namespace forge::os::text
