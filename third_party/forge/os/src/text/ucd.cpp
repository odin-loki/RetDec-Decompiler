// forge::os::text: UCD property tables and lookups.
#include "internal.hpp"
#include "ucd_data.inc"
#include <algorithm>
#include <cctype>
#include <cstring>

namespace forge::os::text {
namespace detail {
static_assert(sizeof(props) == sizeof(ucd::record));
const props& prop(cp_t c) {
    if (c > 0x10FFFF) c = 0x10FFFE;   // out of range: treat like a noncharacter
    unsigned b = ucd::stage1[c >> ucd::block_shift];
    return reinterpret_cast<const props&>(ucd::records[ucd::stage2[(b << ucd::block_shift) | (c & ((1u << ucd::block_shift) - 1))]]);
}
template <std::size_t N> static int find_idx(const unsigned (&k)[N], cp_t c) {
    auto it = std::lower_bound(std::begin(k), std::end(k), unsigned(c));
    return (it != std::end(k) && *it == c) ? int(it - std::begin(k)) : -1;
}
template <std::size_t N, std::size_t M> static cp_t map1(const unsigned (&k)[N], const unsigned (&v)[M], cp_t c) {
    int i = find_idx(k, c); return i < 0 ? c : cp_t(v[i]);
}
template <std::size_t N, std::size_t M, std::size_t P>
static std::span<const unsigned> mapn(const unsigned (&k)[N], const unsigned (&o)[M], const unsigned (&pool)[P], cp_t c) {
    int i = find_idx(k, c); if (i < 0) return {};
    return {pool + (o[i] >> 6), (o[i] >> 1) & 31};
}
std::span<const unsigned> special_upper(cp_t c) { return mapn(ucd::sp_upper_k, ucd::sp_upper_o, ucd::sp_upper_pool, c); }
std::span<const unsigned> special_lower(cp_t c) { return mapn(ucd::sp_lower_k, ucd::sp_lower_o, ucd::sp_lower_pool, c); }
std::span<const unsigned> special_title(cp_t c) { return mapn(ucd::sp_title_k, ucd::sp_title_o, ucd::sp_title_pool, c); }
std::span<const unsigned> full_fold(cp_t c) { return mapn(ucd::fold_full_k, ucd::fold_full_o, ucd::fold_full_pool, c); }
std::span<const unsigned> decomposition(cp_t c, bool& compat) {
    int i = find_idx(ucd::decomp_k, c); if (i < 0) return {};
    unsigned o = ucd::decomp_o[i]; compat = o & 1;
    return {ucd::decomp_pool + (o >> 6), (o >> 1) & 31};
}
cp_t compose(cp_t a, cp_t b) {
    unsigned long long key = (unsigned long long)a << 21 | b;
    auto it = std::lower_bound(std::begin(ucd::comp_k), std::end(ucd::comp_k), key);
    return (it != std::end(ucd::comp_k) && *it == key) ? cp_t(ucd::comp_v[it - std::begin(ucd::comp_k)]) : 0;
}
cp_t bracket_pair(cp_t c, int& type) {
    int i = find_idx(ucd::bracket_k, c); if (i < 0) { type = 0; return c; }
    type = int(ucd::bracket_v[i] & 3); return cp_t(ucd::bracket_v[i] >> 2);
}
const ducet_view& ducet() {
    static const ducet_view v{ucd::ducet_key_o, ucd::ducet_key_pool, ucd::ducet_ce_o, ucd::ducet_ce_pool,
        {reinterpret_cast<const ducet_view::implicit*>(ucd::implicit_ranges), std::size(ucd::implicit_ranges)}};
    return v;
}
std::span<const char* const> script_names() { return ucd::script_names; }
std::span<const char* const> gc_names() { return ucd::gc_names; }
std::span<const char* const> lb_names() { return ucd::lb_names; }
std::span<const char* const> flag_names() { return ucd::flag_names; }
std::vector<std::size_t> cp_byte_offsets(std::string_view s) {
    std::vector<std::size_t> o; o.reserve(s.size() + 1);
    std::size_t i = 0; while (i < s.size()) { o.push_back(i); decode_utf8(s, i); }
    o.push_back(s.size()); return o;
}
}  // namespace detail

using namespace detail;
const char* unicode_version() { return ucd::unicode_version; }
cp_t simple_upper(cp_t c) { return map1(ucd::upper_k, ucd::upper_v, c); }
cp_t simple_lower(cp_t c) { return map1(ucd::lower_k, ucd::lower_v, c); }
cp_t simple_title(cp_t c) { int i = find_idx(ucd::title_k, c); return i < 0 ? simple_upper(c) : cp_t(ucd::title_v[i]); }
cp_t simple_fold(cp_t c) { return map1(ucd::fold_k, ucd::fold_v, c); }
cp_t mirror(cp_t c) { return map1(ucd::mirror_k, ucd::mirror_v, c); }
category general_category(cp_t c) { return category(prop(c).gc); }
const char* category_name(category g) { return ucd::gc_names[int(g)]; }
std::string_view script(cp_t c) { return ucd::script_names[prop(c).script]; }
int combining_class(cp_t c) { return prop(c).ccc; }
std::string_view east_asian_width(cp_t c) { return ucd::eaw_names[prop(c).eaw]; }
std::string_view bidi_class_name(cp_t c) { return ucd::bidi_names[prop(c).bidi]; }
std::string_view line_break_class(cp_t c) { return ucd::lb_names[prop(c).lb]; }
static bool gc_in(cp_t c, category a, category b) { auto g = general_category(c); return g >= a && g <= b; }
bool is_letter(cp_t c) { return gc_in(c, category::Lu, category::Lo); }
bool is_mark(cp_t c) { return gc_in(c, category::Mn, category::Me); }
bool is_number(cp_t c) { return gc_in(c, category::Nd, category::No); }
bool is_digit(cp_t c) { return general_category(c) == category::Nd; }
bool is_punct(cp_t c) { return gc_in(c, category::Pc, category::Po); }
bool is_symbol(cp_t c) { return gc_in(c, category::Sm, category::So); }
bool is_alpha(cp_t c) { return prop(c).flags & F_Alpha; }
bool is_upper(cp_t c) { return prop(c).flags & F_Upper; }
bool is_lower(cp_t c) { return prop(c).flags & F_Lower; }
bool is_space(cp_t c) { return prop(c).flags & F_Space; }
bool is_cased(cp_t c) { return prop(c).flags & F_Cased; }
bool is_case_ignorable(cp_t c) { return prop(c).flags & F_CaseIgn; }
bool is_emoji(cp_t c) { return prop(c).flags & F_Emoji; }
bool is_emoji_presentation(cp_t c) { return prop(c).flags & F_EmojiPres; }
bool is_extended_pictographic(cp_t c) { return prop(c).flags & F_ExtPict; }
bool is_ideographic(cp_t c) { return prop(c).flags & F_Ideo; }
bool is_default_ignorable(cp_t c) { return prop(c).flags & F_DefIgn; }
bool is_noncharacter(cp_t c) { return prop(c).flags & F_NonChar; }
bool is_word_char(cp_t c) {
    return is_alpha(c) || is_mark(c) || general_category(c) == category::Nd || general_category(c) == category::Pc || c == 0x200C || c == 0x200D;
}
std::optional<int> digit_value(cp_t c) {
    if (!is_digit(c)) return std::nullopt;
    // Nd characters come in runs of complete 0..9 sequences (UnicodeData invariant), so count from the run start.
    cp_t s = c; while (s > 0 && is_digit(s - 1)) --s;
    return int((c - s) % 10);
}

static std::string norm_name(std::string_view n) {
    std::string r; for (char ch : n) if (ch != '_' && ch != ' ' && ch != '-') r += char(std::tolower((unsigned char)ch)); return r;
}
bool has_property(cp_t c, std::string_view name, bool* known) {
    if (known) *known = true;
    std::string n(name);
    if (auto eq = n.find_first_of("=:"); eq != std::string::npos) {
        auto k = norm_name(n.substr(0, eq)); auto v = n.substr(eq + 1);
        if (k == "script" || k == "sc" || k == "scriptextensions" || k == "scx") return norm_name(script(c)) == norm_name(v);
        if (k == "generalcategory" || k == "gc") n = v;
        else if (k == "lb" || k == "linebreak") return norm_name(line_break_class(c)) == norm_name(v);
        else if (k == "ea" || k == "eastasianwidth") return norm_name(east_asian_width(c)) == norm_name(v);
        else if (k == "bc" || k == "bidiclass") return norm_name(bidi_class_name(c)) == norm_name(v);
        else { if (known) *known = false; return false; }
    }
    auto g = general_category(c); auto gn = std::string_view(category_name(g));
    if (n.size() == 2 && n == gn) return true;
    if (n.size() == 1 && n[0] == gn[0]) return true;
    if (n.size() <= 2 && std::isupper((unsigned char)n[0])) {
        for (auto x : ucd::gc_names) if (n == x || (n.size() == 1 && n[0] == x[0])) return false;
        if (n == "LC" || n == "L&") return g == category::Lu || g == category::Ll || g == category::Lt;
    }
    static const std::pair<const char*, const char*> long_gc[] = {
        {"letter", "L"}, {"casedletter", "LC"}, {"uppercaseletter", "Lu"}, {"lowercaseletter", "Ll"}, {"titlecaseletter", "Lt"},
        {"modifierletter", "Lm"}, {"otherletter", "Lo"}, {"mark", "M"}, {"combiningmark", "M"}, {"nonspacingmark", "Mn"},
        {"spacingmark", "Mc"}, {"enclosingmark", "Me"}, {"number", "N"}, {"decimalnumber", "Nd"}, {"digit", "Nd"},
        {"letternumber", "Nl"}, {"othernumber", "No"}, {"punctuation", "P"}, {"punct", "P"}, {"connectorpunctuation", "Pc"},
        {"dashpunctuation", "Pd"}, {"openpunctuation", "Ps"}, {"closepunctuation", "Pe"}, {"initialpunctuation", "Pi"},
        {"finalpunctuation", "Pf"}, {"otherpunctuation", "Po"}, {"symbol", "S"}, {"mathsymbol", "Sm"}, {"currencysymbol", "Sc"},
        {"modifiersymbol", "Sk"}, {"othersymbol", "So"}, {"separator", "Z"}, {"spaceseparator", "Zs"}, {"lineseparator", "Zl"},
        {"paragraphseparator", "Zp"}, {"other", "C"}, {"control", "Cc"}, {"cntrl", "Cc"}, {"format", "Cf"}, {"surrogate", "Cs"},
        {"privateuse", "Co"}, {"unassigned", "Cn"}};
    auto nn = norm_name(n);
    for (auto& [l, s] : long_gc) if (nn == l) return has_property(c, s, nullptr);
    if (nn == "any") return true;
    if (nn == "ascii") return c < 0x80;
    if (nn == "assigned") return g != category::Cn;
    if (nn == "alphabetic" || nn == "alpha") return is_alpha(c);
    if (nn == "lowercase" || nn == "lower") return is_lower(c);
    if (nn == "uppercase" || nn == "upper") return is_upper(c);
    if (nn == "whitespace" || nn == "space" || nn == "wspace") return is_space(c);
    if (nn == "word" || nn == "w") return is_word_char(c);
    for (std::size_t i = 0; i < std::size(ucd::flag_names); ++i) if (norm_name(ucd::flag_names[i]) == nn) return prop(c).flags & (1u << i);
    for (auto s : ucd::script_names) if (norm_name(s) == nn) return norm_name(script(c)) == nn;
    // ISO 15924 short codes for the common scripts.
    static const std::pair<const char*, const char*> iso[] = {{"latn", "latin"}, {"grek", "greek"}, {"cyrl", "cyrillic"}, {"arab", "arabic"},
        {"hebr", "hebrew"}, {"hani", "han"}, {"hira", "hiragana"}, {"kana", "katakana"}, {"hang", "hangul"}, {"thai", "thai"},
        {"deva", "devanagari"}, {"zyyy", "common"}, {"zinh", "inherited"}, {"zzzz", "unknown"}};
    for (auto& [a, b] : iso) if (nn == a) return norm_name(script(c)) == b;
    if (known) *known = false; return false;
}
}  // namespace forge::os::text
