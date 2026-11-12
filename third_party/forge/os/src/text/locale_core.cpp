// forge::os::locale: CLDR data access, BCP 47 tags, OS detection and region data.
#include "locale_internal.hpp"
#include "cldr_data.inc"
#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <mutex>
#if defined(_WIN32)
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  include <windows.h>
#elif defined(__ANDROID__)
#  include <sys/system_properties.h>
#endif
#include <clocale>

namespace forge::os::locale {
namespace detail {
namespace {
int key_id(std::string_view k) {
    unsigned lo = 0, hi = cldr::key_count;
    while (lo < hi) {
        unsigned mid = (lo + hi) / 2; std::string_view m = cldr::str(cldr::key_offsets[mid]);
        if (m < k) lo = mid + 1; else hi = mid;
    }
    return (lo < cldr::key_count && std::string_view(cldr::str(cldr::key_offsets[lo])) == k) ? int(lo) : -1;
}
const cldr::locale_table* table(std::string_view tag) {
    for (auto& t : cldr::locales) if (tag == t.tag) return &t;
    return nullptr;
}
std::optional<std::string_view> in_table(const cldr::locale_table* t, int id) {
    if (!t || id < 0) return std::nullopt;
    auto e = std::lower_bound(t->entries, t->entries + t->count, id, [](const cldr::entry& a, int k) { return int(a.key) < k; });
    if (e != t->entries + t->count && int(e->key) == id) return std::string_view(cldr::str(e->value));
    return std::nullopt;
}
}  // namespace
std::vector<std::string> cldr_keys_with_prefix(std::string_view prefix) {
    std::vector<std::string> r;
    for (unsigned i = 0; i < cldr::key_count; ++i) { std::string_view k = cldr::str(cldr::key_offsets[i]); if (k.starts_with(prefix)) r.emplace_back(k); }
    return r;
}
std::optional<std::string_view> find(std::string_view bundled, std::string_view key) {
    int id = key_id(key); if (id < 0) return std::nullopt;
    std::string b(bundled);
    while (true) {
        if (auto v = in_table(table(b), id)) return v;
        auto dash = b.rfind('-'); if (dash == std::string::npos) break;
        b.resize(dash);
    }
    if (b != "en") if (auto v = in_table(table("en"), id)) return v;
    return std::nullopt;
}
std::string_view get(std::string_view bundled, std::string_view key) { return find(bundled, key).value_or(std::string_view{}); }
std::optional<std::string_view> supp(std::string_view key) { return in_table(table("root"), key_id(key)); }
std::string replace_all(std::string s, std::string_view from, std::string_view to) {
    if (from.empty()) return s;
    for (std::size_t p = 0; (p = s.find(from, p)) != std::string::npos; p += to.size()) s.replace(p, from.size(), to);
    return s;
}
std::string sub(std::string_view pattern, std::string_view a, std::string_view b) {
    std::string r; r.reserve(pattern.size() + a.size() + b.size());
    for (std::size_t i = 0; i < pattern.size(); ++i) {
        if (pattern[i] == '{' && i + 2 < pattern.size() && pattern[i + 2] == '}' && (pattern[i + 1] == '0' || pattern[i + 1] == '1')) { r += pattern[i + 1] == '0' ? a : b; i += 2; }
        else r += pattern[i];
    }
    return r;
}
std::string region_of(std::string_view locale) {
    auto t = maximize(parse_tag(locale.empty() ? std::string_view(current()) : locale));
    return t.region.empty() ? "001" : t.region;
}
std::string resolve(std::string_view locale, category c) {
    std::string cur; if (locale.empty()) { cur = current(c); locale = cur; }
    return best_bundled(locale);
}
}  // namespace detail
using namespace detail;

// ---------------------------------------------------------------- tags
namespace {
std::string lower(std::string_view s) { std::string r(s); for (auto& c : r) c = char(std::tolower((unsigned char)c)); return r; }
std::string upper(std::string_view s) { std::string r(s); for (auto& c : r) c = char(std::toupper((unsigned char)c)); return r; }
bool alpha(std::string_view s) { return std::all_of(s.begin(), s.end(), [](char c) { return std::isalpha((unsigned char)c); }); }
bool digits(std::string_view s) { return std::all_of(s.begin(), s.end(), [](char c) { return std::isdigit((unsigned char)c); }); }
}  // namespace

tag parse_tag(std::string_view s) {
    tag t;
    if (auto p = s.find_first_of(".@"); p != std::string_view::npos) {   // POSIX codeset / modifier
        std::string_view mod = s.find('@') != std::string_view::npos ? s.substr(s.find('@') + 1) : std::string_view{};
        s = s.substr(0, p);
        if (mod == "latin") t.script = "Latn"; else if (mod == "cyrillic") t.script = "Cyrl";
    }
    if (s.empty() || s == "C" || s == "POSIX") { t.language = "en"; return t; }
    std::vector<std::string> parts; std::string cur;
    for (char c : s) { if (c == '-' || c == '_') { parts.push_back(cur); cur.clear(); } else cur += c; }
    parts.push_back(cur);
    std::size_t i = 0;
    t.language = lower(parts[i++]);
    static const std::pair<const char*, const char*> aliases[] = {{"iw", "he"}, {"in", "id"}, {"ji", "yi"}, {"jw", "jv"}, {"no", "nb"}, {"tl", "fil"}, {"mo", "ro"}, {"sh", "sr"}};
    for (auto& [a, b] : aliases) if (t.language == a) t.language = b;
    if (t.language == "nb" && s.starts_with("no")) {}
    if (i < parts.size() && parts[i].size() == 4 && alpha(parts[i])) { t.script = upper(parts[i].substr(0, 1)) + lower(parts[i].substr(1)); ++i; }
    if (i < parts.size() && ((parts[i].size() == 2 && alpha(parts[i])) || (parts[i].size() == 3 && digits(parts[i])))) { t.region = upper(parts[i]); ++i; }
    while (i < parts.size() && (parts[i].size() >= 5 || (parts[i].size() == 4 && std::isdigit((unsigned char)parts[i][0])))) t.variants.push_back(lower(parts[i++]));
    while (i < parts.size()) {
        std::string ext = lower(parts[i++]);
        if (ext == "u") {
            while (i < parts.size() && parts[i].size() > 1) {
                std::string k = lower(parts[i++]), v;
                while (i < parts.size() && parts[i].size() > 2) { if (!v.empty()) v += '-'; v += lower(parts[i++]); }
                t.unicode.push_back({k, v});
            }
        } else { while (i < parts.size() && parts[i].size() > 1) ++i; }
    }
    return t;
}
std::string tag::str() const {
    std::string r = language.empty() ? "und" : language;
    if (!script.empty()) r += "-" + script;
    if (!region.empty()) r += "-" + region;
    for (auto& v : variants) r += "-" + v;
    if (!unicode.empty()) { r += "-u"; for (auto& [k, v] : unicode) { r += "-" + k; if (!v.empty()) r += "-" + v; } }
    return r;
}
std::string_view tag::keyword(std::string_view k) const { for (auto& [a, b] : unicode) if (a == k) return b; return {}; }
std::string canonicalize(std::string_view s) { return parse_tag(s).str(); }

tag maximize(const tag& in) {
    tag t = in;
    auto apply = [&](std::string_view v) {
        auto x = parse_tag(v);
        if (t.language.empty() || t.language == "und") t.language = x.language;
        if (t.script.empty()) t.script = x.script;
        if (t.region.empty()) t.region = x.region;
    };
    std::vector<std::string> keys;
    if (!t.script.empty() && !t.region.empty()) keys.push_back(t.language + "-" + t.script);
    if (!t.region.empty()) keys.push_back(t.language + "-" + t.region);
    if (!t.script.empty()) keys.push_back(t.language + "-" + t.script);
    keys.push_back(t.language);
    if (!t.region.empty()) keys.push_back("und-" + t.region);
    for (auto& k : keys) if (auto v = supp("likely/" + k)) { apply(*v); if (!t.script.empty() && !t.region.empty()) break; }
    return t;
}
tag minimize(const tag& in) {
    tag full = maximize(in), t = in;
    for (auto cand : {tag{full.language}, tag{full.language, "", full.region}, tag{full.language, full.script}}) {
        auto m = maximize(cand);
        if (m.script == full.script && m.region == full.region) { cand.variants = in.variants; cand.unicode = in.unicode; return cand; }
    }
    return t;
}
std::vector<std::string> bundled_locales() {
    std::vector<std::string> r; for (auto& t : cldr::locales) if (std::strcmp(t.tag, "root")) r.push_back(t.tag); return r;
}
std::string best_bundled(std::string_view locale) {
    tag t = parse_tag(locale); tag m = maximize(t);
    auto has = [](std::string_view x) { for (auto& l : cldr::locales) if (x == l.tag) return true; return false; };
    if (!t.region.empty() && has(t.language + "-" + t.region)) return t.language + "-" + t.region;
    if (t.language == "zh") return (m.script == "Hant") ? "zh-Hant" : "zh";
    if (t.language == "en" && !m.region.empty()) {
        static const char* us_like[] = {"US", "PR", "GU", "AS", "UM", "VI", "MH", "MP", "PH"};
        if (std::find_if(std::begin(us_like), std::end(us_like), [&](const char* r) { return m.region == r; }) == std::end(us_like)) return "en-GB";
    }
    if (t.language == "es" && !m.region.empty() && m.region != "ES" && m.region != "GQ") return "es-MX";
    if (t.language == "pt" && !m.region.empty() && m.region != "BR") return "pt-PT";
    if (t.language == "fr" && m.region == "CA") return "fr-CA";
    if (t.language == "de" && (m.region == "CH" || m.region == "LI")) return "de-CH";
    if (t.language == "nn" || t.language == "no") return "nb";
    if (has(t.language)) return t.language;
    return "en";
}
std::vector<std::string> fallback_chain(std::string_view locale) {
    tag t = parse_tag(locale); std::vector<std::string> r;
    auto push = [&](std::string s) { if (std::find(r.begin(), r.end(), s) == r.end()) r.push_back(std::move(s)); };
    if (!t.script.empty() && !t.region.empty()) push(t.language + "-" + t.script + "-" + t.region);
    if (!t.script.empty()) push(t.language + "-" + t.script);
    if (!t.region.empty()) push(t.language + "-" + t.region);
    push(t.language); push("root");
    return r;
}
std::string negotiate(std::span<const std::string> req, std::span<const std::string> avail, std::string_view fb) {
    for (auto& r : req) {   // exact canonical match first, then language/script-compatible
        auto rc = canonicalize(r);
        for (auto& a : avail) if (canonicalize(a) == rc) return a;
        auto rm = maximize(parse_tag(r));
        for (auto& a : avail) { auto am = maximize(parse_tag(a)); if (am.language == rm.language && am.script == rm.script && am.region == rm.region) return a; }
        for (auto& a : avail) { auto am = maximize(parse_tag(a)); if (am.language == rm.language && am.script == rm.script) return a; }
    }
    return std::string(fb);
}

// ---------------------------------------------------------------- detection
namespace {
std::mutex g_mu; std::string g_current, g_android; std::vector<std::string> g_android_pref;
std::string env(const char* n) { const char* v = std::getenv(n); return v ? v : ""; }
}  // namespace
void set_current(std::string_view l) { std::lock_guard lk(g_mu); g_current = l.empty() ? "" : canonicalize(l); }
void android_set_locale(std::string_view l, std::span<const std::string> pref) {
    std::lock_guard lk(g_mu); g_android = canonicalize(l); g_android_pref.clear(); for (auto& p : pref) g_android_pref.push_back(canonicalize(p));
}
std::string current() { return current(category::messages); }
std::string current(category c) {
    { std::lock_guard lk(g_mu); if (!g_current.empty()) return g_current; }
    return system_locale(c);
}
namespace {
#if defined(_WIN32)
std::string user_info(LCTYPE t) {
    wchar_t b[128]; int n = GetLocaleInfoEx(LOCALE_NAME_USER_DEFAULT, t, b, 128); if (n <= 1) return {};
    int m = WideCharToMultiByte(CP_UTF8, 0, b, n - 1, nullptr, 0, nullptr, nullptr); std::string s(std::size_t(m > 0 ? m : 0), '\0');
    if (m > 0) WideCharToMultiByte(CP_UTF8, 0, b, n - 1, s.data(), m, nullptr, nullptr);
    return s;
}
#endif
// 'H'/'h'/'K'/'k' outside quotes in a time pattern -> CLDR hour cycle keyword.
std::string hc_of(std::string_view p) {
    bool q = false;
    for (char c : p) { if (c == '\'') q = !q; else if (!q) { if (c == 'H') return "h23"; if (c == 'h') return "h12"; if (c == 'K') return "h11"; if (c == 'k') return "h24"; } }
    return {};
}
}  // namespace
namespace detail {
// User customisations made in the OS settings (Windows Region, macOS/iOS Language & Region), read once.
const os_overrides& user_overrides() {
    static const os_overrides o = [] {
        os_overrides r;
#if defined(_WIN32)
        // Only what the user changed: each value is compared with the locale's own (LOCALE_NOUSEROVERRIDE).
        auto changed = [](LCTYPE t) { auto v = user_info(t); return v == user_info(t | LOCALE_NOUSEROVERRIDE) ? std::string() : v; };
        static const char* wd[] = {"mon", "tue", "wed", "thu", "fri", "sat", "sun"};   // LOCALE_IFIRSTDAYOFWEEK: 0 = Monday
        if (auto f = changed(LOCALE_IFIRSTDAYOFWEEK); f.size() == 1 && f[0] >= '0' && f[0] <= '6') r.fw = wd[f[0] - '0'];
        if (auto t = changed(LOCALE_STIMEFORMAT); !t.empty() && hc_of(t) != hc_of(user_info(LOCALE_STIMEFORMAT | LOCALE_NOUSEROVERRIDE))) r.hc = hc_of(t);
        r.decimal = changed(LOCALE_SDECIMAL); r.group = changed(LOCALE_STHOUSAND);
#elif defined(__APPLE__)
        std::string fmt; apple_user_overrides(r.fw, fmt, r.decimal, r.group); r.hc = hc_of(fmt);
#endif
        return r;
    }();
    return o;
}
std::optional<std::string> user_symbol(std::string_view name) {
    { std::lock_guard lk(g_mu); if (!g_current.empty() || !g_android.empty()) return std::nullopt; }
#if !defined(_WIN32)
    for (auto n : {"LC_ALL", "LC_NUMERIC", "LANG"}) { auto v = env(n); if (!v.empty() && v != "C" && v != "POSIX" && v != "C.UTF-8") return std::nullopt; }   // locale chosen by the environment
#endif
    auto& o = user_overrides();
    if (name == "decimal" && !o.decimal.empty()) return o.decimal;
    if (name == "group" && !o.group.empty()) return o.group;
    return std::nullopt;
}
}  // namespace detail
std::string system_locale() { return system_locale(category::messages); }
std::string system_locale(category c) {
    { std::lock_guard lk(g_mu); if (!g_android.empty()) return g_android; }
    std::string base;
#if defined(_WIN32)
    wchar_t buf[LOCALE_NAME_MAX_LENGTH];
    if (GetUserDefaultLocaleName(buf, LOCALE_NAME_MAX_LENGTH) > 0) { std::string s; for (wchar_t* p = buf; *p; ++p) s += char(*p); base = canonicalize(s); }
    else return "en";
#else
    // POSIX precedence: LC_ALL, then the category's own variable, then LANG.
    const char* var = c == category::time ? "LC_TIME" : c == category::numeric ? "LC_NUMERIC" : "LC_MESSAGES";
    for (auto n : {"LC_ALL", var, "LANG"}) { auto v = env(n); if (!v.empty() && v != "C" && v != "POSIX" && v != "C.UTF-8") return canonicalize(v); }
#  if defined(__APPLE__)
    if (auto a = apple_system_locale(); !a.empty()) base = canonicalize(a);
#  elif defined(__ANDROID__)
    char v[PROP_VALUE_MAX] = {};
    if (__system_property_get("persist.sys.locale", v) > 0 || __system_property_get("ro.product.locale", v) > 0) return canonicalize(v);
#  endif
    if (base.empty()) {
        int lc = c == category::time ? LC_TIME : c == category::numeric ? LC_NUMERIC : LC_MESSAGES;
        if (const char* l = std::setlocale(lc, nullptr); l && std::strcmp(l, "C") && std::strcmp(l, "POSIX")) return canonicalize(l);
        return "en";
    }
#endif
    // Time formatting follows the user's OS choices (24-hour clock, first weekday) as -u-hc / -u-fw keywords, added
    // only where they differ from the CLDR default for the locale. Separators are applied by number formatting.
    if (c == category::time) {
        auto& o = user_overrides(); tag t = parse_tag(base); bool add = false;
        if (!o.hc.empty() && t.keyword("hc").empty()) {
            static const char* hn[] = {"h11", "h12", "h23", "h24"};
            if (o.hc != hn[int(preferred_hour_cycle(base))]) { t.unicode.push_back({"hc", o.hc}); add = true; }
        }
        if (!o.fw.empty() && t.keyword("fw").empty()) {
            static const char* wn[] = {"sun", "mon", "tue", "wed", "thu", "fri", "sat"};
            if (o.fw != wn[int(first_day_of_week(base))]) { t.unicode.push_back({"fw", o.fw}); add = true; }
        }
        if (add) return t.str();
    }
    return base;
}
std::vector<std::string> preferred_languages() {
    std::vector<std::string> r;
    { std::lock_guard lk(g_mu); if (!g_android_pref.empty()) return g_android_pref; }
#if defined(_WIN32)
    ULONG num = 0, len = 0;
    if (GetUserPreferredUILanguages(MUI_LANGUAGE_NAME, &num, nullptr, &len) && len) {
        std::wstring buf(len, L'\0');
        if (GetUserPreferredUILanguages(MUI_LANGUAGE_NAME, &num, buf.data(), &len)) {
            std::string cur; for (wchar_t c : buf) { if (c) cur += char(c); else if (!cur.empty()) { r.push_back(canonicalize(cur)); cur.clear(); } }
        }
    }
#else
    if (auto l = env("LANGUAGE"); !l.empty()) {
        std::string cur; for (char c : l + ":") { if (c == ':') { if (!cur.empty()) r.push_back(canonicalize(cur)); cur.clear(); } else cur += c; }
    }
#  if defined(__APPLE__)
    if (r.empty()) for (auto& a : apple_preferred_languages()) r.push_back(canonicalize(a));
#  endif
#endif
    auto sys = system_locale();
    if (std::find(r.begin(), r.end(), sys) == r.end()) r.push_back(sys);
    return r;
}
std::string display_name(std::string_view language, std::string_view in_locale) {
    auto b = resolve(in_locale); auto c = canonicalize(language);
    if (auto v = find(b, "lang/" + c)) return std::string(*v);
    auto t = parse_tag(c);
    if (auto v = find(b, "lang/" + t.language)) return std::string(*v) + (t.region.empty() ? "" : " (" + t.region + ")");
    return c;
}

// ---------------------------------------------------------------- region data
namespace {
std::string_view region_value(std::string_view prefix, std::string_view locale) {
    auto r = region_of(locale);
    if (auto v = supp(std::string(prefix) + r)) return *v;
    return supp(std::string(prefix) + "001").value_or("");
}
weekday wd(std::string_view s) {
    static const char* n[] = {"sun", "mon", "tue", "wed", "thu", "fri", "sat"};
    for (int i = 0; i < 7; ++i) if (s == n[i]) return weekday(i);
    return weekday::mon;
}
}  // namespace
weekday first_day_of_week(std::string_view l) {
    std::string cur; if (l.empty()) { cur = current(category::time); l = cur; }
    auto t = parse_tag(l);
    if (auto fw = t.keyword("fw"); !fw.empty()) return wd(fw);
    return wd(region_value("week/firstDay/", l));
}
int min_days_in_first_week(std::string_view l) {
    std::string cur; if (l.empty()) { cur = current(category::time); l = cur; }
    auto v = region_value("week/minDays/", l); return v.empty() ? 1 : std::atoi(std::string(v).c_str()); }
std::pair<weekday, weekday> weekend(std::string_view l) { return {wd(region_value("week/weekendStart/", l)), wd(region_value("week/weekendEnd/", l))}; }
measurement measurement_system(std::string_view l) {
    auto t = parse_tag(l.empty() ? std::string_view(current()) : l);
    if (auto ms = t.keyword("ms"); !ms.empty()) return ms == "ussystem" ? measurement::us : ms == "uksystem" ? measurement::uk : measurement::metric;
    auto v = region_value("measure/", l); return v == "US" ? measurement::us : v == "UK" ? measurement::uk : measurement::metric;
}
std::string paper_size(std::string_view l) { return std::string(region_value("paper/", l)); }
hour_cycle preferred_hour_cycle(std::string_view l) {
    std::string cur; if (l.empty()) { cur = current(category::time); l = cur; }
    auto t = parse_tag(l);
    if (auto hc = t.keyword("hc"); !hc.empty()) return hc == "h11" ? hour_cycle::h11 : hc == "h12" ? hour_cycle::h12 : hc == "h24" ? hour_cycle::h24 : hour_cycle::h23;
    auto p = get(resolve(l), "cal/time/short");
    for (std::size_t i = 0; i < p.size(); ++i) {
        if (p[i] == '\'') { auto e = p.find('\'', i + 1); if (e == std::string_view::npos) break; i = e; continue; }
        if (p[i] == 'h') return hour_cycle::h12; if (p[i] == 'H') return hour_cycle::h23; if (p[i] == 'K') return hour_cycle::h11; if (p[i] == 'k') return hour_cycle::h24;
    }
    return hour_cycle::h23;
}
std::string currency_for_region(std::string_view r) {
    std::string reg = (r.size() == 2 && alpha(r)) ? upper(r) : region_of(r);
    auto t = parse_tag(r); if (auto cu = t.keyword("cu"); !cu.empty()) return upper(cu);
    return std::string(supp("curregion/" + reg).value_or("USD"));
}
}  // namespace forge::os::locale
