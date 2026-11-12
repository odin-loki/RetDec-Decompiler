// forge::os::locale: plural rules, number / percent / currency / compact formatting and parsing.
#include "locale_internal.hpp"
#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace forge::os::locale {
using namespace detail;

// ---------------------------------------------------------------- plural rules
namespace {
struct operands { double n = 0; long double i = 0; int v = 0, w = 0; long double f = 0, t = 0; int e = 0; };
operands make_operands(std::string_view s) {
    operands o; std::string str(s);
    if (!str.empty() && (str[0] == '-' || str[0] == '+')) str.erase(0, 1);
    if (auto ep = str.find_first_of("ec"); ep != std::string::npos) { o.e = std::atoi(str.c_str() + ep + 1); str.resize(ep); }
    auto dot = str.find('.');
    std::string ip = str.substr(0, dot), fp = dot == std::string::npos ? "" : str.substr(dot + 1);
    if (o.e > 0) {   // compact exponent shifts digits into the integer part
        for (int k = 0; k < o.e; ++k) { if (!fp.empty()) { ip += fp[0]; fp.erase(0, 1); } else ip += '0'; }
    }
    o.i = std::strtold(ip.empty() ? "0" : ip.c_str(), nullptr);
    o.v = int(fp.size()); o.f = fp.empty() ? 0 : std::strtold(fp.c_str(), nullptr);
    std::string tz = fp; while (!tz.empty() && tz.back() == '0') tz.pop_back();
    o.w = int(tz.size()); o.t = tz.empty() ? 0 : std::strtold(tz.c_str(), nullptr);
    { std::string whole = (ip.empty() ? std::string("0") : ip) + (fp.empty() ? std::string() : "." + fp); o.n = double(std::strtold(whole.c_str(), nullptr)); }
    return o;
}
struct rule_eval {
    std::string_view s; std::size_t p = 0; const operands& o;
    void ws() { while (p < s.size() && s[p] == ' ') ++p; }
    bool kw(std::string_view k) { ws(); if (s.substr(p, k.size()) == k) { p += k.size(); return true; } return false; }
    long double num() { ws(); std::size_t st = p; while (p < s.size() && (std::isdigit((unsigned char)s[p]) || (s[p] == '.' && p + 1 < s.size() && std::isdigit((unsigned char)s[p + 1])))) ++p; return std::strtold(std::string(s.substr(st, p - st)).c_str(), nullptr); }
    long double operand() {
        ws(); char c = p < s.size() ? s[p++] : 'n';
        long double v = c == 'n' ? (long double)o.n : c == 'i' ? o.i : c == 'v' ? o.v : c == 'w' ? o.w : c == 'f' ? o.f : c == 't' ? o.t : (c == 'c' || c == 'e') ? o.e : 0;
        if (kw("%")) { auto m = num(); v = std::fmod(v, m); }
        return v;
    }
    bool relation() {
        long double v = operand(); bool neg;
        if (kw("!=")) neg = true; else if (kw("=")) neg = false; else if (kw("not in") || kw("not within")) neg = true; else if (kw("in") || kw("within") || kw("is not")) neg = false; else { kw("is"); neg = false; }
        bool hit = false;
        do {
            long double a = num(), b = a;
            if (kw("..")) b = num();
            if (a == b ? v == a : (v >= a && v <= b && v == std::floor(v))) hit = true;
        } while (kw(","));
        return hit != neg;
    }
    bool and_cond() { bool r = relation(); while (kw("and")) r = relation() && r; return r; }
    bool cond() { ws(); if (p >= s.size()) return true; bool r = and_cond(); while (kw("or")) r = and_cond() || r; return r; }
};
std::string plural_lang(std::string_view locale, bool ordinal) {
    std::string cur; if (locale.empty()) { cur = current(); locale = cur; }
    auto t = parse_tag(locale);
    const char* kind = ordinal ? "ordinal" : "cardinal";
    for (auto cand : {t.language + "-" + t.region, t.language + "_" + t.region, t.language})
        if (supp(std::string("plural/") + kind + "/" + cand + "/other")) return cand;
    return "root";
}
}  // namespace
const char* plural_name(plural p) { static const char* n[] = {"zero", "one", "two", "few", "many", "other"}; return n[int(p)]; }
plural plural_category(std::string_view number, std::string_view locale, bool ordinal) {
    auto lang = plural_lang(locale, ordinal); auto o = make_operands(number);
    const char* kind = ordinal ? "ordinal" : "cardinal";
    for (int k = 0; k < 5; ++k) {
        auto r = supp(std::string("plural/") + kind + "/" + lang + "/" + plural_name(plural(k)));
        if (r && !r->empty()) { rule_eval ev{*r, 0, o}; if (ev.cond()) return plural(k); }
    }
    return plural::other;
}
plural plural_category(double n, std::string_view locale, bool ordinal) {
    char b[64];
    if (n == std::floor(n) && std::fabs(n) < 1e18) std::snprintf(b, sizeof b, "%.0f", n);
    else { std::snprintf(b, sizeof b, "%.6f", n); std::string s(b); while (s.back() == '0') s.pop_back(); return plural_category(s, locale, ordinal); }
    return plural_category(std::string_view(b), locale, ordinal);
}
std::vector<plural> plural_categories(std::string_view locale, bool ordinal) {
    auto lang = plural_lang(locale, ordinal); std::vector<plural> r;
    const char* kind = ordinal ? "ordinal" : "cardinal";
    for (int k = 0; k < 6; ++k) if (supp(std::string("plural/") + kind + "/" + lang + "/" + plural_name(plural(k)))) r.push_back(plural(k));
    return r;
}

// ---------------------------------------------------------------- number core
namespace {
struct digit_set { const char* name; char32_t zero; };
constexpr digit_set digit_sets[] = {{"arab", 0x660}, {"arabext", 0x6F0}, {"deva", 0x966}, {"beng", 0x9E6}, {"guru", 0xA66}, {"gujr", 0xAE6},
    {"orya", 0xB66}, {"tamldec", 0xBE6}, {"telu", 0xC66}, {"knda", 0xCE6}, {"mlym", 0xD66}, {"thai", 0xE50}, {"laoo", 0xED0}, {"tibt", 0xF20},
    {"mymr", 0x1040}, {"khmr", 0x17E0}, {"mong", 0x1810}, {"fullwide", 0xFF10}, {"hanidec", 0}};
}  // namespace
std::string localize_digits(std::string_view a, std::string_view numbering) {
    if (numbering.empty() || numbering == "latn") return std::string(a);
    static const char32_t hanidec[] = {0x3007, 0x4E00, 0x4E8C, 0x4E09, 0x56DB, 0x4E94, 0x516D, 0x4E03, 0x516B, 0x4E5D};
    for (auto& d : digit_sets) if (numbering == d.name) {
        std::string r;
        for (char c : a) {
            if (c >= '0' && c <= '9') text::append_utf8(r, d.zero ? d.zero + char32_t(c - '0') : hanidec[c - '0']);
            else r += c;
        }
        return r;
    }
    return std::string(a);
}
namespace detail {
std::string numbering_for(std::string_view bundled, std::string_view req, std::string_view locale_tag) {
    if (req == "native") return std::string(get(bundled, "num/native"));
    if (!req.empty() && req != "default") return std::string(req);
    auto t = parse_tag(locale_tag); if (auto nu = t.keyword("nu"); !nu.empty()) return std::string(nu);
    // CLDR keeps Arabic-Indic digits as the default in a few Arabic regions whose data we fold into "ar".
    if (t.language == "ar" && bundled == "ar" && (t.region == "EG" || t.region == "SD" || t.region == "SY" || t.region == "IQ" || t.region == "JO" || t.region == "KW")) return "arab";
    return std::string(get(bundled, "num/system"));
}
pattern_info parse_number_pattern(std::string_view p) {
    pattern_info pi;
    auto semi = std::string_view::npos;
    { bool q = false; for (std::size_t i = 0; i < p.size(); ++i) { if (p[i] == '\'') q = !q; else if (p[i] == ';' && !q) { semi = i; break; } } }
    auto body = [&](std::string_view s, std::string& pre, std::string& suf, bool main) {
        std::size_t a = 0; bool q = false;
        auto lit = [&](std::string_view part) { std::string r; bool qq = false; for (std::size_t i = 0; i < part.size(); ++i) { if (part[i] == '\'') { if (i + 1 < part.size() && part[i + 1] == '\'') { r += '\''; ++i; } else qq = !qq; } else r += part[i]; } (void)qq; return r; };
        while (a < s.size()) { if (s[a] == '\'') q = !q; else if (!q && std::strchr("#0,.@", s[a])) break; ++a; }
        std::size_t b = a; while (b < s.size() && std::strchr("#0,.@E+", s[b])) ++b;
        pre = lit(s.substr(0, a)); suf = lit(s.substr(b));
        if (!main) return;
        auto num = s.substr(a, b - a);
        if (auto e = num.find('E'); e != std::string_view::npos) num = num.substr(0, e);
        auto dot = num.find('.');
        auto ip = num.substr(0, dot), fp = dot == std::string_view::npos ? std::string_view{} : num.substr(dot + 1);
        pi.min_int = int(std::count(ip.begin(), ip.end(), '0')); if (pi.min_int == 0 && fp.empty()) pi.min_int = 1;
        pi.min_frac = int(std::count(fp.begin(), fp.end(), '0')); pi.max_frac = int(fp.size());
        auto lc = ip.rfind(',');
        if (lc != std::string_view::npos) {
            pi.group1 = int(ip.size() - lc - 1);
            auto pc = ip.rfind(',', lc - 1);
            pi.group2 = pc != std::string_view::npos ? int(lc - pc - 1) : pi.group1;
        }
    };
    body(p.substr(0, semi), pi.prefix, pi.suffix, true);
    if (semi != std::string_view::npos) { body(p.substr(semi + 1), pi.neg_prefix, pi.neg_suffix, false); pi.has_neg = true; }
    return pi;
}
std::string format_with_pattern(double v, const pattern_info& pi, std::string_view b, const number_options& o, std::string_view nu, bool user_syms) {
    bool latn_syms = nu == "latn" && get(b, "num/system") != "latn";
    bool arab_syms = nu == "arab" && get(b, "num/system") != "arab";   // CLDR arab symbols (ar-EG and friends)
    auto sym = [&](const char* k) {
        if (arab_syms) {
            std::string_view kk = k;
            if (kk == "decimal") return std::string("\xD9\xAB"); if (kk == "group") return std::string("\xD9\xAC");
            if (kk == "percentSign") return std::string("\xD9\xAA\xD8\x9C"); if (kk == "minusSign") return std::string("\xD8\x9C-");
            if (kk == "plusSign") return std::string("\xD8\x9C+");
        }
        if (user_syms && !latn_syms && !arab_syms) if (auto u = user_symbol(k)) return *u;   // OS-customised separators
        std::string key = std::string(latn_syms ? "latn/sym/" : "sym/") + k; auto r = find(b, key); return std::string(r ? *r : get(b, std::string("sym/") + k)); };
    if (std::isnan(v)) return sym("nan");
    bool neg = std::signbit(v) && v != 0; double a = std::fabs(v);
    std::string digits;
    int minf = o.min_fraction >= 0 ? o.min_fraction : pi.min_frac, maxf = o.max_fraction >= 0 ? o.max_fraction : std::max(pi.max_frac, minf);
    if (maxf < minf) maxf = minf;
    if (std::isinf(a)) digits = "\x01";
    else {
        char buf[512];
        if (o.max_significant > 0) {
            std::snprintf(buf, sizeof buf, "%.*e", o.max_significant - 1, a);
            double r = std::strtod(buf, nullptr); a = r;
            int mag = a == 0 ? 0 : int(std::floor(std::log10(a)));
            int fd = std::max(0, o.max_significant - 1 - mag);
            std::snprintf(buf, sizeof buf, "%.*f", fd, a);
            std::string s(buf);
            if (s.find('.') != std::string::npos) { while (s.back() == '0') s.pop_back(); if (s.back() == '.') s.pop_back(); }
            if (minf > 0) { auto d = s.find('.'); int have = d == std::string::npos ? 0 : int(s.size() - d - 1); if (!have) s += '.'; for (; have < minf; ++have) s += '0'; }
            digits = s;
        } else {
            double scaled = a * std::pow(10.0, maxf);
            if (o.round != rounding::half_even && scaled < 9e15) {
                double r;
                switch (o.round) {
                case rounding::half_up: r = std::floor(scaled + 0.5); break;
                case rounding::half_down: r = std::ceil(scaled - 0.5); break;
                case rounding::ceiling: r = neg ? std::floor(scaled) : std::ceil(scaled); break;
                case rounding::floor: r = neg ? std::ceil(scaled) : std::floor(scaled); break;
                case rounding::down: r = std::trunc(scaled); break;
                default: r = std::ceil(scaled - 1e-9); break;
                }
                a = r / std::pow(10.0, maxf);
            }
            std::snprintf(buf, sizeof buf, "%.*f", maxf, a);
            std::string s(buf);
            if (auto d = s.find('.'); d != std::string::npos) {
                std::size_t keep = d + 1 + std::size_t(minf);
                while (s.size() > keep && s.back() == '0') s.pop_back();
                if (s.back() == '.') s.pop_back();
            }
            digits = s;
        }
    }
    if (neg && digits.find_first_not_of("0.") == std::string::npos && digits != "\x01") neg = false;   // -0.00 -> 0.00
    std::string out;
    if (digits == "\x01") out = sym("infinity");
    else {
        auto d = digits.find('.');
        std::string ip = digits.substr(0, d), fp = d == std::string::npos ? "" : digits.substr(d + 1);
        int mi = std::max(o.min_integer, pi.min_int);
        while (int(ip.size()) < mi) ip.insert(0, "0");
        if (mi == 0 && ip == "0" && !fp.empty()) ip.clear();
        std::string grp = sym("group"), dec = sym("decimal");
        int min_group = std::atoi(std::string(get(b, "num/mingroup")).c_str()); if (min_group < 1) min_group = 1;
        std::string g;
        if (o.grouping && pi.group1 > 0 && int(ip.size()) >= pi.group1 + min_group) {
            int n = int(ip.size()), first = pi.group1, rest = pi.group2 ? pi.group2 : pi.group1;
            std::vector<std::string> parts; int end = n;
            parts.push_back(ip.substr(std::size_t(end - first))); end -= first;
            while (end > 0) { int st = std::max(0, end - rest); parts.push_back(ip.substr(std::size_t(st), std::size_t(end - st))); end = st; }
            for (auto it = parts.rbegin(); it != parts.rend(); ++it) { if (!g.empty()) g += grp; g += *it; }
        } else g = ip;
        out = localize_digits(g, nu);
        if (!fp.empty()) out += dec + localize_digits(fp, nu);
    }
    auto expand = [&](const std::string& affix) {
        std::string r;
        for (std::size_t i = 0; i < affix.size(); ++i) {
            char c = affix[i];
            if (c == '%') r += sym("percentSign");
            else if (c == '-') r += sym("minusSign");
            else if (c == '+') r += sym("plusSign");
            else if (affix.compare(i, 3, "\xE2\x80\xB0") == 0) { r += sym("perMille"); i += 2; }
            else r += c;
        }
        return r;
    };
    bool show_plus = o.sign == sign_display::always || (o.sign == sign_display::except_zero && v != 0 && !neg);
    if (o.sign == sign_display::never) neg = false;
    if (neg) {
        if (pi.has_neg) return expand(pi.neg_prefix) + out + expand(pi.neg_suffix);
        return sym("minusSign") + expand(pi.prefix) + out + expand(pi.suffix);
    }
    return (show_plus ? sym("plusSign") : "") + expand(pi.prefix) + out + expand(pi.suffix);
}
}  // namespace detail

std::string number_symbol(std::string_view name, std::string_view locale) { return std::string(get(resolve(locale, category::numeric), "sym/" + std::string(name))); }
std::string format_number(double v, std::string_view locale, const number_options& o) {
    auto b = resolve(locale, category::numeric); std::string tag(locale.empty() ? current(category::numeric) : std::string(locale));
    return format_with_pattern(v, parse_number_pattern(get(b, "fmt/decimal")), b, o, numbering_for(b, o.numbering, tag), locale.empty());
}
std::string format_integer(std::int64_t v, std::string_view locale, const number_options& o) {
    number_options x = o; x.max_fraction = 0; x.min_fraction = 0;
    if (std::llabs(v) < (1LL << 53)) return format_number(double(v), locale, x);
    // Large values: format digits exactly through the pattern with grouping on a string.
    auto s = std::to_string(v); auto b = resolve(locale, category::numeric);
    auto pi = parse_number_pattern(get(b, "fmt/decimal"));
    std::string grp(get(b, "sym/group")), out; bool neg = s[0] == '-'; if (neg) s.erase(0, 1);
    for (std::size_t i = 0; i < s.size(); ++i) { if (o.grouping && i && pi.group1 && (s.size() - i) % std::size_t(pi.group1) == 0) out += grp; out += s[i]; }
    return (neg ? std::string(get(b, "sym/minusSign")) : "") + out;
}
std::string format_percent(double f, std::string_view locale, const number_options& o) {
    auto b = resolve(locale, category::numeric); std::string tag(locale.empty() ? current(category::numeric) : std::string(locale));
    return format_with_pattern(f * 100, parse_number_pattern(get(b, "fmt/percent")), b, o, numbering_for(b, o.numbering, tag), locale.empty());
}
std::string format_permille(double f, std::string_view locale, const number_options& o) {
    auto b = resolve(locale, category::numeric); auto pi = parse_number_pattern(get(b, "fmt/percent"));
    pi.prefix = replace_all(pi.prefix, "%", "\xE2\x80\xB0"); pi.suffix = replace_all(pi.suffix, "%", "\xE2\x80\xB0");
    return format_with_pattern(f * 1000, pi, b, o, numbering_for(b, o.numbering, locale.empty() ? current(category::numeric) : std::string(locale)), locale.empty());
}
std::string format_scientific(double v, std::string_view locale, int maxf) {
    auto b = resolve(locale, category::numeric);
    if (v == 0 || !std::isfinite(v)) return format_number(v, locale);
    int e = int(std::floor(std::log10(std::fabs(v)))); double m = v / std::pow(10.0, e);
    if (std::fabs(m) >= 10 - 0.5 * std::pow(10.0, -maxf)) { m /= 10; ++e; }
    number_options o; o.max_fraction = maxf; o.grouping = false;
    auto nu = numbering_for(b, "", locale.empty() ? current(category::numeric) : std::string(locale));
    std::string es = std::to_string(std::abs(e));
    return format_number(m, locale, o) + std::string(get(b, "sym/exponential")) + (e < 0 ? std::string(get(b, "sym/minusSign")) : "") + localize_digits(es, nu);
}
namespace {
std::string compact_core(double v, std::string_view b, std::string_view locale, const char* family, int maxsig, std::string* pattern_out) {
    double a = std::fabs(v);
    if (a < 1000) { number_options o; o.max_significant = a >= 100 ? 3 : maxsig; o.max_fraction = 0; if (a < 100) o.max_significant = std::max(maxsig, 2); return format_number(v, locale, o); }
    int mag = int(std::floor(std::log10(a)));
    // Rounding may bump magnitude (999950 -> 1.0M).
    for (int pass = 0; pass < 2; ++pass) {
        std::string base = std::string(family) + "/1" + std::string(std::size_t(std::min(mag, 14)), '0');
        auto other = find(b, base + "-count-other");
        if (!other || *other == "0") break;
        std::string pat(*other);
        int zeros = 0; { bool q = false; for (char c : pat) { if (c == '\'') q = !q; else if (c == '0' && !q) ++zeros; } }
        double scaled = v / std::pow(10.0, std::min(mag, 14) - zeros + 1);
        number_options o; o.max_significant = std::max(maxsig, zeros); o.grouping = false;
        char tmp[64]; std::snprintf(tmp, sizeof tmp, "%.*g", o.max_significant, std::fabs(scaled));
        if (std::strtod(tmp, nullptr) >= std::pow(10.0, zeros) && pass == 0) { ++mag; continue; }
        auto num = format_number(scaled, locale, o);
        auto cat = plural_category(std::string_view(tmp), locale);
        if (auto pp = find(b, base + "-count-" + plural_name(cat))) pat = *pp;
        std::string r; bool q = false, placed = false;
        for (std::size_t i = 0; i < pat.size(); ++i) {
            if (pat[i] == '\'') { if (i + 1 < pat.size() && pat[i + 1] == '\'') { r += '\''; ++i; } else q = !q; continue; }
            if (!q && pat[i] == '0') { if (!placed) { r += num; placed = true; } continue; }
            r += pat[i];
        }
        if (pattern_out) *pattern_out = pat;
        return r;
    }
    number_options o; o.max_fraction = 0; return format_number(v, locale, o);
}
}  // namespace
std::string format_compact(double v, std::string_view locale, compact_width w, int maxsig) {
    auto b = resolve(locale, category::numeric);
    return compact_core(v, b, locale, w == compact_width::long_ ? "fmt/compact-long" : "fmt/compact-short", maxsig, nullptr);
}
int currency_digits(std::string_view c, bool cash) {
    auto v = supp("curdigits/" + text::to_upper(c)); if (!v) v = supp("curdigits/DEFAULT");
    if (!v) return 2;
    int d[4] = {2, 0, 2, 0}; std::sscanf(std::string(*v).c_str(), "%d,%d,%d,%d", &d[0], &d[1], &d[2], &d[3]);
    return cash ? d[2] : d[0];
}
std::string currency_symbol(std::string_view c, std::string_view locale, bool narrow) {
    auto b = resolve(locale, category::numeric); auto code = text::to_upper(c);
    if (narrow) if (auto n = find(b, "cur/" + code + "/symbol-alt-narrow")) return std::string(*n);
    if (auto s = find(b, "cur/" + code + "/symbol")) return std::string(*s);
    return code;
}
std::string currency_name(std::string_view c, std::string_view locale, std::optional<double> count) {
    auto b = resolve(locale, category::numeric); auto code = text::to_upper(c);
    if (count) if (auto n = find(b, "cur/" + code + "/displayName-count-" + plural_name(plural_category(*count, locale)))) return std::string(*n);
    if (count) if (auto n = find(b, "cur/" + code + "/displayName-count-other")) return std::string(*n);
    if (auto n = find(b, "cur/" + code + "/displayName")) return std::string(*n);
    return code;
}
std::string format_currency(double amount, std::string_view c, std::string_view locale, currency_display d, bool accounting, const number_options& o) {
    auto b = resolve(locale, category::numeric); auto code = text::to_upper(c);
    number_options x = o;
    int digs = currency_digits(code);
    if (x.min_fraction < 0) x.min_fraction = digs;
    if (x.max_fraction < 0) x.max_fraction = std::max(digs, x.min_fraction);
    std::string tag(locale.empty() ? current(category::numeric) : std::string(locale));
    auto nu = numbering_for(b, x.numbering, tag);
    if (d == currency_display::name) {
        auto pi = parse_number_pattern(get(b, "fmt/decimal"));
        auto num = format_with_pattern(amount, pi, b, x, nu, locale.empty());
        char tmp[64]; std::snprintf(tmp, sizeof tmp, "%.*f", x.min_fraction, std::fabs(amount));
        auto cat = plural_category(std::string_view(tmp), locale);
        auto up = find(b, std::string("fmt/currency-unit/") + plural_name(cat)); if (!up) up = find(b, "fmt/currency-unit/other");
        return sub(up ? *up : "{0} {1}", num, currency_name(code, locale, std::strtod(tmp, nullptr)));
    }
    std::string symbol = d == currency_display::code ? code : currency_symbol(code, locale, d == currency_display::narrow_symbol);
    std::string pat(get(b, accounting ? "fmt/accounting" : "fmt/currency"));
    // alphaNextToNumber: "US$"-style symbols ending in a letter get the spaced pattern when one exists.
    bool alpha_sym = false;
    { std::size_t i = 0; char32_t last = 0, first = 0; while (i < symbol.size()) { char32_t ch = text::decode_utf8(symbol, i); if (!first) first = ch; last = ch; }
      auto pos = pat.find("\xC2\xA4"); bool before = pos != std::string::npos && pos < pat.find_first_of("#0");
      alpha_sym = text::is_letter(before ? last : first); }
    if (alpha_sym && !accounting) if (auto ap = find(b, "fmt/currency-alpha")) pat = *ap;
    if (alpha_sym && accounting && d == currency_display::code) {
        // ensure separation for ISO codes: "USD 1.00"
        auto pos = pat.find("\xC2\xA4"); if (pos != std::string::npos && pos + 2 < pat.size() && (pat[pos + 2] == '#' || pat[pos + 2] == '0')) pat.insert(pos + 2, "\xC2\xA0");
    }
    auto pi = parse_number_pattern(pat);
    auto put = [&](std::string& a) { a = replace_all(a, "\xC2\xA4", "\x01"); };
    put(pi.prefix); put(pi.suffix); put(pi.neg_prefix); put(pi.neg_suffix);
    auto s = format_with_pattern(amount, pi, b, x, nu, locale.empty());
    return replace_all(s, "\x01", symbol);
}
std::string format_currency_compact(double amount, std::string_view c, std::string_view locale) {
    auto b = resolve(locale, category::numeric); std::string pat;
    if (std::fabs(amount) < 1000) return format_currency(amount, c, locale);
    auto s = compact_core(amount, b, locale, "fmt/currency-compact", 2, &pat);
    // The compact currency patterns embed the number; prefix/suffix the symbol through the pattern's ¤.
    return replace_all(s, "\xC2\xA4", currency_symbol(c, locale));
}
std::string format_bytes(std::uint64_t bytes, std::string_view locale, bool binary) {
    static const char* dec[] = {"byte", "kilobyte", "megabyte", "gigabyte", "terabyte", "petabyte"};
    static const char* bin[] = {"B", "KiB", "MiB", "GiB", "TiB", "PiB"};
    double base = binary ? 1024 : 1000; double v = double(bytes); int k = 0;
    while (v >= base && k < 5) { v /= base; ++k; }
    number_options o; o.max_fraction = k == 0 ? 0 : (v < 10 ? 1 : 0);
    if (binary) return format_number(v, locale, o) + "\xC2\xA0" + bin[k];
    return format_unit(v, dec[k], locale, unit_width::short_, o);
}

// ---------------------------------------------------------------- parsing
namespace {
std::string normalize_number_text(std::string_view s, std::string_view b, bool& percent, bool& permille, bool& paren) {
    std::string dec(get(b, "sym/decimal")), grp(get(b, "sym/group")), ldec(get(b, "latn/sym/decimal")), lgrp(get(b, "latn/sym/group"));
    if (ldec.empty()) ldec = dec; if (lgrp.empty()) lgrp = grp;
    std::string out; std::size_t i = 0;
    while (i < s.size()) {
        auto rest = s.substr(i);
        auto starts = [&](std::string_view x) { return !x.empty() && rest.starts_with(x); };
        if (starts(dec) || (starts(ldec) && ldec != grp)) { out += '.'; i += starts(dec) ? dec.size() : ldec.size(); continue; }
        if (starts(grp) || starts(lgrp)) { i += starts(grp) ? grp.size() : lgrp.size(); continue; }
        std::size_t st = i; char32_t c = text::decode_utf8(s, i);
        if (auto dv = text::digit_value(c)) { out += char('0' + *dv); continue; }
        if (c == '-' || c == 0x2212 || c == 0x2010 || c == 0x2011 || c == 0x2013) { out += '-'; continue; }
        if (c == '+') { out += '+'; continue; }
        if (c == '(' ) { paren = true; continue; }
        if (c == ')') continue;
        if (c == '%' || c == 0x066A || c == 0xFF05) { percent = true; continue; }
        if (c == 0x2030 || c == 0x0609) { permille = true; continue; }
        if (c == 'E' || c == 'e') { out += 'e'; continue; }
        if (c == 0x0638 || c == 0x0623) { out += 'e'; continue; }   // Arabic exponent marks
        if (text::is_space(c) || c == 0x200E || c == 0x200F || c == 0x061C || c == '\'' || c == 0x2019) continue;
        if (c == '.' && dec != ".") continue;   // stray ASCII grouping dot
        if (c == ',' && dec == ",") { out += '.'; continue; }
        out += "\x02"; (void)st; break;   // unknown character
    }
    return out;
}
}  // namespace
result<double> parse_number(std::string_view s, std::string_view locale) {
    result<double> r; auto b = resolve(locale, category::numeric);
    bool pct = false, pm = false, paren = false;
    auto t = normalize_number_text(s, b, pct, pm, paren);
    if (t.empty() || t.find('\x02') != std::string::npos) { r.err = {EINVAL, "parse_number: not a number: '" + std::string(s) + "'"}; return r; }
    errno = 0; char* end = nullptr; double v = std::strtod(t.c_str(), &end);
    if (end == t.c_str() || *end) { r.err = {EINVAL, "parse_number: not a number: '" + std::string(s) + "'"}; return r; }
    if (pct) v /= 100; if (pm) v /= 1000; if (paren) v = -std::fabs(v);
    r.value = v; return r;
}
result<currency_amount> parse_currency(std::string_view s, std::string_view locale) {
    result<currency_amount> r; auto b = resolve(locale, category::numeric);
    std::string str(s), code;
    static const char* codes[] = {"USD", "EUR", "GBP", "JPY", "CNY", "AUD", "CAD", "CHF", "HKD", "NZD", "SEK", "NOK", "DKK", "PLN", "CZK", "HUF", "RON", "BGN", "RUB", "UAH", "TRY",
        "ILS", "AED", "SAR", "EGP", "INR", "PKR", "BDT", "IDR", "MYR", "SGD", "THB", "VND", "PHP", "KRW", "TWD", "BRL", "MXN", "ARS", "CLP", "COP", "PEN", "ZAR", "NGN", "KES", "IRR", "ISK", "BTC"};
    // Longest symbol/code present wins; the locale's own currency is preferred for ambiguous "$".
    auto home = currency_for_region(locale.empty() ? current(category::numeric) : std::string(locale));
    std::size_t best = 0;
    auto consider = [&](const std::string& c, const std::string& sym) {
        if (sym.empty()) return; auto p = str.find(sym);
        if (p != std::string::npos && (sym.size() > best || (sym.size() == best && c == home))) { best = sym.size(); code = c; }
    };
    for (auto c : codes) { consider(c, c); consider(c, currency_symbol(c, locale)); consider(c, currency_symbol(c, locale, true)); }
    if (code.empty()) { r.err = {EINVAL, "parse_currency: no currency symbol in '" + str + "'"}; return r; }
    for (auto sym : {std::string(code), currency_symbol(code, locale), currency_symbol(code, locale, true)}) if (!sym.empty()) str = replace_all(str, sym, "");
    auto n = parse_number(str, locale);
    if (!n) { r.err = n.err; return r; }
    r.value = {n.value, code}; return r;
}
}  // namespace forge::os::locale
