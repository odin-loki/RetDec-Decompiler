// forge::os::locale: dates and times. LDML pattern formatting/parsing, CLDR styles and skeletons, ISO 8601,
// relative time and durations.
#include "locale_internal.hpp"
#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace forge::os::locale {
using namespace detail;

int datetime::weekday() const { auto d = days_from_civil(year, month, day); return int(((d % 7) + 11) % 7); }
int datetime::day_of_year() const { return int(days_from_civil(year, month, day) - days_from_civil(year, 1, 1)) + 1; }
std::int64_t datetime::local_seconds() const { return days_from_civil(year, month, day) * 86400 + hour * 3600 + minute * 60 + second; }
sys_seconds datetime::to_sys() const { return sys_seconds(std::chrono::seconds(local_seconds() - utc_offset)); }

namespace {
datetime split(std::int64_t local, int ns) {
    datetime d; std::int64_t days = local / 86400, rem = local % 86400; if (rem < 0) { rem += 86400; --days; }
    auto c = civil_from_days(days); d.year = c[0]; d.month = c[1]; d.day = c[2];
    d.hour = int(rem / 3600); d.minute = int(rem / 60 % 60); d.second = int(rem % 60); d.nanosecond = ns; return d;
}
std::pair<std::int64_t, int> secs_ns(std::chrono::system_clock::time_point t) {
    auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(t.time_since_epoch()).count();
    std::int64_t s = ns / 1000000000; std::int64_t r = ns % 1000000000; if (r < 0) { r += 1000000000; --s; }
    return {s, int(r)};
}
}  // namespace
datetime from_time_utc(std::chrono::system_clock::time_point t) { auto [s, ns] = secs_ns(t); auto d = split(s, ns); d.zone = "UTC"; d.zone_abbrev = "UTC"; return d; }
result<datetime> from_time(std::chrono::system_clock::time_point t, std::string_view zone) {
    result<datetime> r; auto [s, ns] = secs_ns(t);
    std::string z = zone.empty() ? local_zone() : std::string(zone);
    auto zi = zone_at(z, sys_seconds(std::chrono::seconds(s)));
    if (!zi) { r.err = zi.err; return r; }
    r.value = split(s + zi.value.offset.count(), ns);
    r.value.utc_offset = int(zi.value.offset.count()); r.value.zone = z; r.value.zone_abbrev = zi.value.abbrev;
    return r;
}
datetime now(std::string_view zone) {
    auto t = std::chrono::system_clock::now();
    auto r = from_time(t, zone); return r ? r.value : from_time_utc(t);
}

// ---------------------------------------------------------------- names
namespace {
const char* day_keys[] = {"sun", "mon", "tue", "wed", "thu", "fri", "sat"};
std::string name_lookup(std::string_view b, std::string_view kind, std::string_view key, std::string_view width, bool standalone) {
    std::string ctx = standalone ? "stand-alone" : "format";
    for (auto c : {ctx, std::string(standalone ? "format" : "stand-alone")})
        if (auto v = find(b, "cal/" + std::string(kind) + "/" + c + "/" + std::string(width) + "/" + std::string(key))) return std::string(*v);
    if (width == "short") return name_lookup(b, kind, key, "abbreviated", standalone);
    return {};
}
}  // namespace
std::string month_name(int m, std::string_view locale, std::string_view width, bool standalone) {
    if (m < 1 || m > 12) return {};
    return name_lookup(resolve(locale, category::time), "months", std::to_string(m), width, standalone);
}
std::string weekday_name(int wd, std::string_view locale, std::string_view width, bool standalone) {
    if (wd < 0 || wd > 6) return {};
    return name_lookup(resolve(locale, category::time), "days", day_keys[wd], width, standalone);
}
std::string day_period(int hour, std::string_view locale, std::string_view width) {
    auto b = resolve(locale, category::time);
    return std::string(get(b, "cal/dayPeriods/" + std::string(width) + (hour < 12 ? "/am" : "/pm")));
}

// ---------------------------------------------------------------- formatting
namespace {
struct ctx { std::string b; std::string tagstr; std::string nu; calendar cal = calendar::gregorian; };
ctx make_ctx(std::string_view locale) {
    ctx c; c.tagstr = locale.empty() ? current(category::time) : std::string(locale); c.b = resolve(c.tagstr);
    c.nu = numbering_for(c.b, {}, c.tagstr);
    auto t = parse_tag(c.tagstr);
    if (auto ca = t.keyword("ca"); !ca.empty()) if (auto k = calendar_from_name(ca)) c.cal = *k;
    return c;
}
std::string pad(std::int64_t v, int n) { std::string s = std::to_string(v < 0 ? -v : v); while (int(s.size()) < n) s.insert(s.begin(), '0'); return v < 0 ? "-" + s : s; }
std::string offset_str(int off, bool colon, bool z_for_zero, bool always_min) {
    if (off == 0 && z_for_zero) return "Z";
    char sign = off < 0 ? '-' : '+'; int a = std::abs(off), h = a / 3600, m = a / 60 % 60;
    std::string r(1, sign); r += pad(h, 2);
    if (m || always_min) { if (colon) r += ':'; r += pad(m, 2); }
    return r;
}
std::string gmt_format(const ctx& c, int off, bool long_) {
    if (off == 0) return std::string(find(c.b, "fmt/misc/gmtZero").value_or("GMT"));
    int a = std::abs(off), h = a / 3600, m = a / 60 % 60;
    std::string hh = (off < 0 ? "-" : "+") + (long_ ? pad(h, 2) : std::to_string(h)) + (long_ || m ? ":" + pad(m, 2) : "");
    return sub(find(c.b, "fmt/misc/gmtFormat").value_or("GMT{0}"), hh);
}
std::string dperiod(const ctx& c, int hour, int n) { const char* w = n <= 3 ? "abbreviated" : n == 4 ? "wide" : "narrow"; return std::string(get(c.b, std::string("cal/dayPeriods/") + w + (hour < 12 ? "/am" : "/pm"))); }
std::string flex_period(const ctx& c, const datetime& d, int n) {
    const char* w = n <= 3 ? "abbreviated" : n == 4 ? "wide" : "narrow";
    std::string pre = std::string("cal/dayPeriods/") + w + "/";
    if (d.hour == 12 && d.minute == 0) if (auto v = find(c.b, pre + "noon")) return std::string(*v);
    if (d.hour == 0 && d.minute == 0) if (auto v = find(c.b, pre + "midnight")) return std::string(*v);
    const char* k = d.hour >= 6 && d.hour < 12 ? "morning1" : d.hour >= 12 && d.hour < 18 ? "afternoon1" : d.hour >= 18 && d.hour < 21 ? "evening1" : "night1";
    if (auto v = find(c.b, pre + k)) return std::string(*v);
    return dperiod(c, d.hour, n);
}

std::string format_field(const ctx& c, const datetime& d, char f, int n) {
    std::string out; bool numeric = true; std::int64_t num = 0; int width = 1;
    cal_date cd = c.cal == calendar::gregorian ? cal_date{d.year, d.month, d.day, d.year > 0 ? 1 : 0} : from_gregorian(c.cal, d.year, d.month, d.day);
    switch (f) {
    case 'G': {
        numeric = false;
        if (c.cal != calendar::gregorian && c.cal != calendar::iso8601) { out = era_name(c.cal, cd.era); break; }
        const char* w = n <= 3 ? "eraAbbr" : n == 4 ? "eraNames" : "eraNarrow";
        out = std::string(get(c.b, std::string("cal/eras/") + w + "/" + (cd.era ? "1" : "0")));
        break;
    }
    case 'y': case 'u': case 'Y': {
        std::int64_t y = f == 'Y' ? to_iso_week(d.year, d.month, d.day).year : (f == 'u' ? d.year : cd.year);
        if (f == 'y' && c.cal == calendar::gregorian && y <= 0) y = 1 - y;
        if (n == 2) { num = ((y % 100) + 100) % 100; width = 2; } else { num = y; width = n; }
        break;
    }
    case 'Q': case 'q': {
        int q = (d.month - 1) / 3 + 1;
        if (n <= 2) { num = q; width = n; break; }
        numeric = false; const char* w = n == 3 ? "abbreviated" : n == 4 ? "wide" : "narrow";
        out = name_lookup(c.b, "quarters", std::to_string(q), w, f == 'q');
        break;
    }
    case 'M': case 'L': {
        int m = c.cal == calendar::gregorian ? d.month : cd.month;
        if (n <= 2) { num = m; width = n; break; }
        numeric = false;
        if (c.cal != calendar::gregorian && c.cal != calendar::buddhist && c.cal != calendar::roc && c.cal != calendar::japanese && c.cal != calendar::iso8601) { out = calendar_month_name(c.cal, cd); break; }
        out = name_lookup(c.b, "months", std::to_string(m), n == 3 ? "abbreviated" : n == 4 ? "wide" : "narrow", f == 'L');
        break;
    }
    case 'd': num = c.cal == calendar::gregorian ? d.day : cd.day; width = n; break;
    case 'D': num = d.day_of_year(); width = n; break;
    case 'F': num = (d.day - 1) / 7 + 1; width = n; break;
    case 'w': num = week_of_year(d.year, d.month, d.day, c.tagstr); width = n; break;
    case 'W': { int first = int(first_day_of_week(c.tagstr)); int wd1 = datetime{d.year, d.month, 1}.weekday(); num = (d.day - 1 + (wd1 - first + 7) % 7) / 7 + 1; width = n; break; }
    case 'E': case 'e': case 'c': {
        int wd = d.weekday();
        if ((f == 'e' || f == 'c') && n <= 2) { num = (wd - int(first_day_of_week(c.tagstr)) + 7) % 7 + 1; width = n; break; }
        numeric = false;
        out = name_lookup(c.b, "days", day_keys[wd], n <= 3 ? "abbreviated" : n == 4 ? "wide" : n == 5 ? "narrow" : "short", f == 'c');
        break;
    }
    case 'a': numeric = false; out = dperiod(c, d.hour, n); break;
    case 'b': case 'B': numeric = false; out = f == 'b' ? (d.hour == 12 && d.minute == 0 ? std::string(find(c.b, "cal/dayPeriods/abbreviated/noon").value_or(dperiod(c, 12, n))) : dperiod(c, d.hour, n)) : flex_period(c, d, n); break;
    case 'h': num = d.hour % 12 == 0 ? 12 : d.hour % 12; width = n; break;
    case 'H': num = d.hour; width = n; break;
    case 'K': num = d.hour % 12; width = n; break;
    case 'k': num = d.hour == 0 ? 24 : d.hour; width = n; break;
    case 'm': num = d.minute; width = n; break;
    case 's': num = d.second; width = n; break;
    case 'S': { std::string frac = pad(d.nanosecond, 9); while (int(frac.size()) < n) frac += '0'; out = frac.substr(0, n); numeric = false; out = localize_digits(out, c.nu); break; }
    case 'A': num = (d.local_seconds() % 86400 + 86400) % 86400 * 1000 + d.nanosecond / 1000000; width = n; break;
    case 'z': numeric = false; out = n <= 3 && !d.zone_abbrev.empty() && !std::isdigit((unsigned char)d.zone_abbrev[0]) && d.zone_abbrev[0] != '+' && d.zone_abbrev[0] != '-' ? d.zone_abbrev : gmt_format(c, d.utc_offset, n == 4); break;
    case 'Z': numeric = false; out = n <= 3 ? offset_str(d.utc_offset, false, false, true) : n == 4 ? gmt_format(c, d.utc_offset, true) : offset_str(d.utc_offset, true, true, true); break;
    case 'O': numeric = false; out = gmt_format(c, d.utc_offset, n == 4); break;
    case 'X': case 'x': numeric = false; out = offset_str(d.utc_offset, n == 3 || n == 5, f == 'X', n >= 2); break;
    case 'v': case 'V': numeric = false;
        if (f == 'V' && n == 2) out = d.zone.empty() ? "UTC" : d.zone;
        else if (f == 'V' && n == 3) { auto z = d.zone; auto p = z.rfind('/'); out = replace_all(p == std::string::npos ? z : z.substr(p + 1), "_", " "); }
        else out = gmt_format(c, d.utc_offset, n == 4);
        break;
    default: numeric = false; out.assign(n, f); break;
    }
    if (numeric) out = localize_digits(pad(num, width), c.nu);
    return out;
}
}  // namespace

std::string format_pattern(const datetime& d, std::string_view p, std::string_view locale) {
    auto c = make_ctx(locale); std::string out;
    for (std::size_t i = 0; i < p.size();) {
        char ch = p[i];
        if (ch == '\'') {
            if (i + 1 < p.size() && p[i + 1] == '\'') { out += '\''; i += 2; continue; }
            ++i;
            while (i < p.size()) {
                if (p[i] == '\'') { if (i + 1 < p.size() && p[i + 1] == '\'') { out += '\''; i += 2; continue; } ++i; break; }
                out += p[i++];
            }
            continue;
        }
        if ((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z')) {
            std::size_t j = i; while (j < p.size() && p[j] == ch) ++j;
            out += format_field(c, d, ch, int(j - i)); i = j; continue;
        }
        out += ch; ++i;
    }
    return out;
}

namespace {
const char* style_name(date_style s) { switch (s) { case date_style::full: return "full"; case date_style::long_: return "long"; case date_style::medium: return "medium"; default: return "short"; } }
// Applies a -u-hc override to a time pattern.
std::string apply_hc(std::string p, std::string_view tagstr) {
    auto hc = parse_tag(tagstr).keyword("hc");
    if (hc.empty()) return p;
    char want = hc == "h11" ? 'K' : hc == "h12" ? 'h' : hc == "h23" ? 'H' : 'k';
    bool twelve = want == 'h' || want == 'K', in_q = false; std::string r;
    for (std::size_t i = 0; i < p.size(); ++i) {
        char ch = p[i];
        if (ch == '\'') { in_q = !in_q; r += ch; continue; }
        if (in_q) { r += ch; continue; }
        if (ch == 'h' || ch == 'H' || ch == 'k' || ch == 'K') { r += want; continue; }
        if (ch == 'a' && !twelve) { while (!r.empty() && (r.back() == ' ' || r.back() == '\xA0')) r.pop_back(); if (r.size() >= 3 && r.compare(r.size() - 3, 3, "\xE2\x80\xAF") == 0) r.resize(r.size() - 3); continue; }
        r += ch;
    }
    if (twelve && r.find('a') == std::string::npos) r += " a";
    return r;
}
}  // namespace
std::string format_date(const datetime& d, std::string_view locale, date_style s) {
    if (s == date_style::none) return {};
    auto c = make_ctx(locale);
    return format_pattern(d, get(c.b, std::string("cal/date/") + style_name(s)), c.tagstr);
}
std::string format_time(const datetime& d, std::string_view locale, date_style s) {
    if (s == date_style::none) return {};
    auto c = make_ctx(locale);
    return format_pattern(d, apply_hc(std::string(get(c.b, std::string("cal/time/") + style_name(s))), c.tagstr), c.tagstr);
}
std::string format_datetime(const datetime& d, std::string_view locale, date_style ds, date_style ts) {
    if (ds == date_style::none) return format_time(d, locale, ts);
    if (ts == date_style::none) return format_date(d, locale, ds);
    auto c = make_ctx(locale);
    return sub(get(c.b, std::string("cal/datetime/") + style_name(ds)), format_time(d, c.tagstr, ts), format_date(d, c.tagstr, ds));
}

// ---------------------------------------------------------------- skeletons
namespace {
// Field class per letter, for skeleton matching.
int field_class(char ch) {
    switch (ch) {
    case 'G': return 0; case 'y': case 'Y': case 'u': return 1; case 'Q': case 'q': return 2; case 'M': case 'L': return 3;
    case 'w': case 'W': return 4; case 'd': case 'D': case 'F': return 5; case 'E': case 'e': case 'c': return 6;
    case 'a': case 'b': case 'B': return 7; case 'h': case 'H': case 'K': case 'k': case 'j': case 'J': case 'C': return 8;
    case 'm': return 9; case 's': return 10; case 'S': return 11; case 'z': case 'Z': case 'O': case 'v': case 'V': case 'X': case 'x': return 12;
    }
    return -1;
}
struct skel { int len[13] = {}; char ch[13] = {}; };
skel parse_skel(std::string_view s) {
    skel k;
    for (char ch : s) { int f = field_class(ch); if (f < 0) continue; if (!k.ch[f]) k.ch[f] = ch; ++k.len[f]; }
    return k;
}
int distance(const skel& want, const skel& have) {
    int d = 0;
    for (int f = 0; f < 13; ++f) {
        if (bool(want.len[f]) != bool(have.len[f])) { if (f == 7 && !want.len[f]) { d += 1; continue; } return 1 << 20; }
        if (!want.len[f]) continue;
        bool wtext = (f == 3 || f == 2) ? want.len[f] >= 3 : false, htext = (f == 3 || f == 2) ? have.len[f] >= 3 : false;
        if (wtext != htext) d += 0x1000;
        if (f == 8 && ((want.ch[f] == 'H' || want.ch[f] == 'k') != (have.ch[f] == 'H' || have.ch[f] == 'k'))) d += 0x10000;
        d += std::abs(want.len[f] - have.len[f]);
    }
    return d;
}
// Stretches fields in a pattern to the requested widths (MMM -> MMMM when MMMM was asked for).
std::string adjust(std::string p, const skel& want) {
    std::string r; bool q = false;
    for (std::size_t i = 0; i < p.size();) {
        char ch = p[i];
        if (ch == '\'') { q = !q; r += ch; ++i; continue; }
        if (q || !std::isalpha((unsigned char)ch)) { r += ch; ++i; continue; }
        std::size_t j = i; while (j < p.size() && p[j] == ch) ++j;
        int n = int(j - i), f = field_class(ch);
        if (f >= 0 && want.len[f]) {
            int w = want.len[f];
            bool text_have = n >= 3, text_want = w >= 3;
            if ((f == 3 || f == 6 || f == 2 || f == 0) && text_have == text_want) n = w;
            else if ((f == 1 || f == 5 || f == 9 || f == 10 || f == 8) && w > n && !(f == 1 && w == 2)) n = w;
            else if (f == 1 && w == 2) n = 2;
            else if (f == 11) n = w;
        }
        r.append(std::size_t(n), ch); i = j;
    }
    return r;
}
std::string best_match(const ctx& c, std::string_view want_s) {
    if (auto v = find(c.b, "cal/skeleton/" + std::string(want_s))) return std::string(*v);
    skel want = parse_skel(want_s);
    // Candidate skeletons: every key of the locale and its parents.
    static thread_local std::vector<std::string> keys;
    keys.clear();
    for (auto& k : cldr_keys_with_prefix("cal/skeleton/")) keys.push_back(k);
    int best = 1 << 20; std::string pat;
    for (auto& k : keys) {
        auto s = std::string_view(k).substr(13);
        int dd = distance(want, parse_skel(s));
        if (dd < best) { auto v = find(c.b, k); if (!v) continue; best = dd; pat = std::string(*v); }
    }
    if (best >= (1 << 20)) return {};
    return adjust(pat, want);
}
}  // namespace
std::string pattern_for_skeleton(std::string_view skeleton, std::string_view locale) {
    auto c = make_ctx(locale);
    // j/J/C: locale preferred hour.
    std::string sk; auto hc = preferred_hour_cycle(c.tagstr);
    char pref = hc == hour_cycle::h23 ? 'H' : hc == hour_cycle::h24 ? 'k' : hc == hour_cycle::h11 ? 'K' : 'h';
    for (char ch : skeleton) sk += (ch == 'j' || ch == 'C') ? pref : ch == 'J' ? 'H' : ch;
    if (pref != 'H' && pref != 'k' && sk.find('h') == std::string::npos && sk.find('K') == std::string::npos) {} 
    std::string date, time;
    for (char ch : sk) { int f = field_class(ch); if (f < 0) continue; (f >= 7 ? time : date) += ch; }
    std::string dp = date.empty() ? "" : best_match(c, date), tp = time.empty() ? "" : best_match(c, time);
    if (!time.empty()) tp = apply_hc(tp, c.tagstr);
    if (date.empty()) return tp;
    if (time.empty()) return dp;
    auto k = parse_skel(date);
    const char* st = k.len[3] >= 4 ? (k.len[6] ? "full" : "long") : k.len[3] == 3 ? "medium" : "short";
    return sub(get(c.b, std::string("cal/datetime/") + st), tp, dp);
}
std::string format_skeleton(const datetime& d, std::string_view skeleton, std::string_view locale) {
    std::string tag = locale.empty() ? current(category::time) : std::string(locale);
    return format_pattern(d, pattern_for_skeleton(skeleton, tag), tag);
}

// ---------------------------------------------------------------- parsing
namespace {
bool istarts(std::string_view s, std::string_view p) {
    if (p.empty() || s.size() < p.size()) return false;
    return text::to_lower(s.substr(0, p.size())) == text::to_lower(p);
}
std::string ascii_digits(std::string_view s) {   // maps any Unicode Nd digit to ASCII
    std::string r; std::size_t i = 0;
    while (i < s.size()) {
        std::size_t st = i; auto cp = text::decode_utf8(s, i);
        if (i == st) ++i;
        auto v = text::digit_value(cp);
        if (v && cp > 0x7F) r += char('0' + *v); else r.append(s.substr(st, i - st));
    }
    return r;
}
}  // namespace
result<datetime> parse_pattern(std::string_view input, std::string_view p, std::string_view locale) {
    result<datetime> r; auto c = make_ctx(locale);
    std::string s = ascii_digits(input); std::size_t pos = 0;
    datetime d; int pm = -1, hour12 = -1; bool have_off = false;
    auto fail = [&](std::string what) { r.err = {EINVAL, "parse_pattern: " + what + " at offset " + std::to_string(pos)}; return r; };
    auto skip_ws = [&] { while (pos < s.size() && (s[pos] == ' ' || s[pos] == '\t')) ++pos; while (pos + 1 < s.size() && (unsigned char)s[pos] == 0xC2 && (unsigned char)s[pos + 1] == 0xA0) pos += 2; while (pos + 2 < s.size() && s.compare(pos, 3, "\xE2\x80\xAF") == 0) pos += 3; };
    auto number = [&](int maxlen, int& out) {
        std::size_t st = pos; bool neg = false; if (pos < s.size() && s[pos] == '-') { neg = true; ++pos; }
        int v = 0, n = 0; while (pos < s.size() && std::isdigit((unsigned char)s[pos]) && n < maxlen) { v = v * 10 + (s[pos++] - '0'); ++n; }
        if (!n) { pos = st; return false; } out = neg ? -v : v; return true;
    };
    for (std::size_t i = 0; i < p.size();) {
        char ch = p[i];
        if (ch == '\'') {
            auto e = p.find('\'', i + 1); auto lit = p.substr(i + 1, (e == std::string_view::npos ? p.size() : e) - i - 1);
            if (lit.empty()) lit = "'";
            if (s.compare(pos, lit.size(), lit) != 0) return fail("expected '" + std::string(lit) + "'");
            pos += lit.size(); i = e == std::string_view::npos ? p.size() : e + 1; continue;
        }
        if (!std::isalpha((unsigned char)ch)) {
            if (ch == ' ' || (unsigned char)ch == 0xC2 || (unsigned char)ch == 0xE2) { skip_ws(); while (i < p.size() && !std::isalpha((unsigned char)p[i]) && p[i] != '\'' && ((unsigned char)p[i] >= 0x80 || p[i] == ' ')) ++i; continue; }
            if (pos >= s.size() || s[pos] != ch) return fail(std::string("expected '") + ch + "'");
            ++pos; ++i; continue;
        }
        std::size_t j = i; while (j < p.size() && p[j] == ch) ++j; int n = int(j - i); i = j;
        bool next_is_digit_field = i < p.size() && std::isalpha((unsigned char)p[i]);
        int maxlen = next_is_digit_field ? std::max(n, 2) : 10;
        int v = 0;
        switch (ch) {
        case 'y': case 'u': case 'Y':
            if (!number(n == 2 ? 2 : maxlen, v)) return fail("expected year");
            d.year = (n == 2 && v < 100) ? (v < 50 ? 2000 + v : 1900 + v) : v; break;
        case 'M': case 'L':
            if (n <= 2) { if (!number(2, v) || v < 1 || v > 12) return fail("expected month"); d.month = v; break; }
            {
                bool ok = false; std::size_t bestlen = 0;
                for (const char* w : {"wide", "abbreviated"}) for (bool sa : {false, true}) for (int m = 1; m <= 12; ++m) {
                    auto nm = name_lookup(c.b, "months", std::to_string(m), w, sa);
                    if (nm.size() > bestlen && istarts(std::string_view(s).substr(pos), nm)) { bestlen = nm.size(); d.month = m; ok = true; }
                }
                if (!ok) return fail("expected month name"); pos += bestlen;
            }
            break;
        case 'd': if (!number(2, v) || v < 1 || v > 31) return fail("expected day"); d.day = v; break;
        case 'E': case 'c': case 'e': {
            if ((ch != 'E') && n <= 2) { if (!number(1, v)) return fail("expected weekday"); break; }
            std::size_t bestlen = 0;
            for (const char* w : {"wide", "abbreviated", "short"}) for (bool sa : {false, true}) for (auto k : day_keys) {
                auto nm = name_lookup(c.b, "days", k, w, sa);
                if (nm.size() > bestlen && istarts(std::string_view(s).substr(pos), nm)) bestlen = nm.size();
            }
            if (!bestlen) return fail("expected weekday name"); pos += bestlen; break;
        }
        case 'a': case 'b': case 'B': {
            std::size_t bestlen = 0;
            for (const char* w : {"abbreviated", "wide", "narrow"}) for (int h : {0, 12}) {
                auto nm = std::string(get(c.b, std::string("cal/dayPeriods/") + w + (h ? "/pm" : "/am")));
                if (nm.size() > bestlen && istarts(std::string_view(s).substr(pos), nm)) { bestlen = nm.size(); pm = h ? 1 : 0; }
            }
            if (!bestlen) { for (auto [nm, h] : {std::pair{"am", 0}, {"pm", 1}, {"a.m.", 0}, {"p.m.", 1}}) if (istarts(std::string_view(s).substr(pos), nm)) { bestlen = std::strlen(nm); pm = h; } }
            if (!bestlen) return fail("expected AM/PM"); pos += bestlen; break;
        }
        case 'h': case 'K': if (!number(2, v)) return fail("expected hour"); hour12 = ch == 'h' ? v % 12 : v; break;
        case 'H': case 'k': if (!number(2, v) || v > 24) return fail("expected hour"); d.hour = v % 24; break;
        case 'm': if (!number(2, v) || v > 59) return fail("expected minute"); d.minute = v; break;
        case 's': if (!number(2, v) || v > 60) return fail("expected second"); d.second = v; break;
        case 'S': { int k = 0; std::int64_t ns = 0; while (pos < s.size() && std::isdigit((unsigned char)s[pos])) { if (k < 9) { ns = ns * 10 + (s[pos] - '0'); ++k; } ++pos; } while (k++ < 9) ns *= 10; d.nanosecond = int(ns); break; }
        case 'X': case 'x': case 'Z': case 'O': case 'z': {
            if (ch == 'X' && pos < s.size() && s[pos] == 'Z') { ++pos; have_off = true; d.utc_offset = 0; break; }
            if (istarts(std::string_view(s).substr(pos), "GMT") || istarts(std::string_view(s).substr(pos), "UTC")) { pos += 3; }
            if (pos < s.size() && (s[pos] == '+' || s[pos] == '-')) {
                int sign = s[pos++] == '-' ? -1 : 1, hh = 0, mm = 0;
                std::size_t st = pos; while (pos < s.size() && std::isdigit((unsigned char)s[pos])) ++pos;
                auto digits = s.substr(st, pos - st);
                if (digits.size() == 4) { hh = std::stoi(digits.substr(0, 2)); mm = std::stoi(digits.substr(2)); }
                else if (!digits.empty()) { hh = std::stoi(digits); if (pos < s.size() && s[pos] == ':') { ++pos; if (!number(2, mm)) return fail("expected offset minutes"); } }
                d.utc_offset = sign * (hh * 3600 + mm * 60);
            }
            have_off = true; break;
        }
        default: return fail(std::string("unsupported field ") + ch);
        }
    }
    if (hour12 >= 0) d.hour = hour12 + (pm == 1 ? 12 : 0);
    else if (pm == 1 && d.hour < 12) d.hour += 12;
    (void)have_off;
    if (d.day > days_in_month(calendar::gregorian, d.year, d.month)) return fail("day out of range");
    r.value = d; return r;
}

result<datetime> parse_iso8601(std::string_view s) {
    result<datetime> r; datetime d; std::size_t pos = 0;
    auto fail = [&](const char* w) { r.err = {EINVAL, std::string("parse_iso8601: ") + w}; return r; };
    auto num = [&](int n, int& out) { if (pos + n > s.size()) return false; int v = 0; for (int i = 0; i < n; ++i) { if (!std::isdigit((unsigned char)s[pos + i])) return false; v = v * 10 + (s[pos + i] - '0'); } out = v; pos += n; return true; };
    int sign = 1; if (pos < s.size() && (s[pos] == '+' || s[pos] == '-')) { sign = s[pos] == '-' ? -1 : 1; ++pos; }
    int y; if (!num(4, y)) return fail("expected year");
    while (pos < s.size() && std::isdigit((unsigned char)s[pos]) && sign != 1) y = y * 10 + (s[pos++] - '0');
    d.year = sign * y;
    bool ext = pos < s.size() && s[pos] == '-'; if (ext) ++pos;
    if (pos < s.size() && s[pos] == 'W') {   // week date
        ++pos; int w, wd = 1; if (!num(2, w)) return fail("expected week");
        if (pos < s.size() && (s[pos] == '-' || std::isdigit((unsigned char)s[pos]))) { if (s[pos] == '-') ++pos; if (!num(1, wd)) return fail("expected weekday"); }
        std::int64_t jan4 = days_from_civil(d.year, 1, 4); int jwd = int(((jan4 % 7) + 10) % 7) + 1;
        auto c = civil_from_days(jan4 - (jwd - 1) + (w - 1) * 7 + (wd - 1)); d.year = c[0]; d.month = c[1]; d.day = c[2];
    } else if (pos < s.size() && std::isdigit((unsigned char)s[pos])) {
        int a; std::size_t st = pos; while (pos < s.size() && std::isdigit((unsigned char)s[pos])) ++pos; std::size_t len = pos - st; pos = st;
        if (len == 3) { num(3, a); auto c = civil_from_days(days_from_civil(d.year, 1, 1) + a - 1); d.month = c[1]; d.day = c[2]; }   // ordinal
        else {
            if (!num(2, d.month)) return fail("expected month");
            if (pos < s.size() && (s[pos] == '-' || (!ext && std::isdigit((unsigned char)s[pos])))) { if (s[pos] == '-') ++pos; if (!num(2, d.day)) return fail("expected day"); }
        }
    }
    if (d.month < 1 || d.month > 12 || d.day < 1 || d.day > days_in_month(calendar::gregorian, d.year, d.month)) return fail("date out of range");
    if (pos < s.size() && (s[pos] == 'T' || s[pos] == 't' || s[pos] == ' ')) {
        ++pos;
        if (!num(2, d.hour)) return fail("expected hour");
        bool c1 = pos < s.size() && s[pos] == ':'; if (c1) ++pos;
        if (pos < s.size() && std::isdigit((unsigned char)s[pos])) {
            if (!num(2, d.minute)) return fail("expected minute");
            if (pos < s.size() && (s[pos] == ':' || std::isdigit((unsigned char)s[pos]))) { if (s[pos] == ':') ++pos; if (!num(2, d.second)) return fail("expected second"); }
        }
        if (pos < s.size() && (s[pos] == '.' || s[pos] == ',')) {
            ++pos; int k = 0; std::int64_t ns = 0;
            while (pos < s.size() && std::isdigit((unsigned char)s[pos])) { if (k < 9) { ns = ns * 10 + (s[pos] - '0'); ++k; } ++pos; }
            if (!k) return fail("expected fraction"); while (k++ < 9) ns *= 10; d.nanosecond = int(ns);
        }
        if (d.hour == 24 && d.minute == 0 && d.second == 0) { d.hour = 0; auto c = civil_from_days(days_from_civil(d.year, d.month, d.day) + 1); d.year = c[0]; d.month = c[1]; d.day = c[2]; }
        if (d.hour > 23 || d.minute > 59 || d.second > 60) return fail("time out of range");
        if (pos < s.size() && (s[pos] == 'Z' || s[pos] == 'z')) { ++pos; d.zone = "UTC"; d.zone_abbrev = "UTC"; }
        else if (pos < s.size() && (s[pos] == '+' || s[pos] == '-')) {
            int sg = s[pos++] == '-' ? -1 : 1, hh, mm = 0;
            if (!num(2, hh)) return fail("expected offset");
            if (pos < s.size() && s[pos] == ':') ++pos;
            if (pos < s.size() && std::isdigit((unsigned char)s[pos]) && !num(2, mm)) return fail("expected offset minutes");
            d.utc_offset = sg * (hh * 3600 + mm * 60);
        }
        if (pos < s.size() && s[pos] == '[') { auto e = s.find(']', pos); if (e == std::string_view::npos) return fail("unterminated zone"); d.zone = s.substr(pos + 1, e - pos - 1); pos = e + 1; }
    }
    if (pos != s.size()) return fail("trailing characters");
    r.value = d; return r;
}
std::string format_iso8601(const datetime& d, bool with_offset) {
    char buf[64];
    std::string y = d.year >= 0 && d.year <= 9999 ? pad(d.year, 4) : (d.year < 0 ? "-" : "+") + pad(std::abs(d.year), 6);
    std::snprintf(buf, sizeof buf, "-%02d-%02dT%02d:%02d:%02d", d.month, d.day, d.hour, d.minute, d.second);
    std::string r = y + buf;
    if (d.nanosecond) { std::string f = pad(d.nanosecond, 9); while (f.size() > 3 && f.substr(f.size() - 3) == "000") f.resize(f.size() - 3); r += "." + f; }
    if (with_offset) r += offset_str(d.utc_offset, true, true, true);
    return r;
}

// ---------------------------------------------------------------- relative time and durations
std::string format_relative(double v, rel_unit u, std::string_view locale, rel_width w, bool numeric) {
    static const char* names[] = {"second", "minute", "hour", "day", "week", "month", "quarter", "year"};
    auto b = resolve(locale, category::time);
    std::string f = std::string("rel/") + names[int(u)] + (w == rel_width::short_ ? "-short" : w == rel_width::narrow ? "-narrow" : "");
    auto look = [&](const std::string& k) { if (auto x = find(b, f + k)) return std::optional<std::string_view>(x); auto base = std::string("rel/") + names[int(u)]; return find(b, base + k); };
    if (!numeric && v == std::floor(v) && std::fabs(v) <= 3) if (auto x = look("/rel/" + std::to_string(int(v)))) return std::string(*x);
    bool past = v < 0 || (v == 0 && std::signbit(v));
    double a = std::fabs(v);
    number_options o; o.max_fraction = a == std::floor(a) ? 0 : 1;
    auto num = format_number(a, locale, o);
    std::string plain = a == std::floor(a) ? std::to_string(std::int64_t(a)) : format_number(a, "en", number_options{.max_fraction = 1, .grouping = false});
    auto cat = plural_name(plural_category(std::string_view(plain), locale));
    std::string dir = past ? "/past/" : "/future/";
    auto pat = look(dir + cat); if (!pat) pat = look(dir + "other");
    if (!pat) return (past ? "-" : "+") + num + " " + names[int(u)];
    return sub(*pat, num);
}
std::string format_relative_time(sys_seconds target, sys_seconds nowt, std::string_view locale, rel_width w) {
    std::int64_t d = (target - nowt).count(), a = std::llabs(d); int sg = d < 0 ? -1 : 1;
    if (a < 45) return format_relative(0.0 * sg, rel_unit::second, locale, w, false);
    if (a < 45 * 60) return format_relative(double(sg * std::max<std::int64_t>(1, (a + 30) / 60)), rel_unit::minute, locale, w, true);
    if (a < 22 * 3600) return format_relative(double(sg * ((a + 1800) / 3600)), rel_unit::hour, locale, w, true);
    // Calendar-day differences for day-sized spans.
    std::int64_t day_diff = (target.time_since_epoch().count() / 86400) - (nowt.time_since_epoch().count() / 86400);
    if (a < 6 * 86400 + 43200) return format_relative(double(day_diff ? day_diff : sg), rel_unit::day, locale, w, false);
    if (a < 26 * 86400) return format_relative(double(sg * ((a + 3 * 86400) / (7 * 86400))), rel_unit::week, locale, w, false);
    if (a < 320 * 86400) return format_relative(double(sg * std::max<std::int64_t>(1, (a + 15 * 86400) / 2629746)), rel_unit::month, locale, w, false);
    return format_relative(double(sg * std::max<std::int64_t>(1, (a + 182 * 86400) / 31556952)), rel_unit::year, locale, w, false);
}
std::string format_duration(std::chrono::seconds dur, std::string_view locale, unit_width w, int max_units) {
    std::int64_t s = dur.count(); bool neg = s < 0; if (neg) s = -s;
    struct part { const char* unit; std::int64_t size; };
    static const part parts[] = {{"duration-day", 86400}, {"duration-hour", 3600}, {"duration-minute", 60}, {"duration-second", 1}};
    std::vector<std::string> items;
    for (auto& p : parts) {
        std::int64_t v = s / p.size; s %= p.size;
        if (v && int(items.size()) < std::max(1, max_units)) items.push_back(format_unit(double(v), p.unit, locale, w));
        else if (v && !items.empty()) break;
        if (!items.empty() && int(items.size()) >= max_units) break;
    }
    if (items.empty()) items.push_back(format_unit(0, "duration-second", locale, w));
    auto r = format_list(items, locale, list_type::unit, w == unit_width::long_ ? list_width::wide : w == unit_width::short_ ? list_width::short_ : list_width::narrow);
    return neg ? "-" + r : r;
}
}  // namespace forge::os::locale
