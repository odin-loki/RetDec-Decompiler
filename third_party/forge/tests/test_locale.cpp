// forge::os::locale unit tests (bundled CLDR data, no OS state assumed beyond an optional zoneinfo directory).
#include <forge/os/locale.hpp>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>
#if !defined(_WIN32)
#include <sys/stat.h>
#endif
namespace L = forge::os::locale;
static int fails = 0, checks = 0;
#define CHECK(x) do { ++checks; if (!(x)) { ++fails; std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #x); } } while (0)
#define EQ(a, b) do { ++checks; std::string _a = (a), _b = (b); if (_a != _b) { ++fails; std::printf("FAIL %s:%d: %s\n   got [%s]\n  want [%s]\n", __FILE__, __LINE__, #a, _a.c_str(), _b.c_str()); } } while (0)
#define EQN(a, b) do { ++checks; auto _a = (a); auto _b = (b); if (!(_a == _b)) { ++fails; std::printf("FAIL %s:%d: %s == %s (got %lld)\n", __FILE__, __LINE__, #a, #b, (long long)_a); } } while (0)
static const std::string NNBSP = "\xE2\x80\xAF", NBSP = "\xC2\xA0";

static void tags() {
    EQ(L::canonicalize("EN_au.UTF-8"), "en-AU");
    EQ(L::canonicalize("zh-hans_cn"), "zh-Hans-CN");
    EQ(L::canonicalize("C"), "en");
    EQ(L::canonicalize("iw"), "he");
    auto t = L::parse_tag("de-DE-u-co-phonebk-hc-h12");
    EQ(t.language, "de"); EQ(t.region, "DE"); EQ(std::string(t.keyword("hc")), "h12");
    EQ(L::maximize(L::parse_tag("zh-TW")).str(), "zh-Hant-TW");
    EQ(L::maximize(L::parse_tag("sr")).str(), "sr-Cyrl-RS");
    EQ(L::minimize(L::parse_tag("en-Latn-US")).str(), "en");
    EQ(L::best_bundled("en-NZ"), "en-GB");
    EQ(L::best_bundled("es-AR"), "es-MX");
    EQ(L::best_bundled("zh-TW"), "zh-Hant");
    EQ(L::best_bundled("pt-AO"), "pt-PT");
    EQ(L::best_bundled("xx"), "en");
    CHECK(L::bundled_locales().size() >= 50);
    std::vector<std::string> req{"fr-CA", "en"}, avail{"en", "fr", "de"};
    EQ(L::negotiate(req, avail), "fr");
    EQ(L::display_name("de", "fr"), "allemand");
    EQ(L::display_name("ja", "en"), "Japanese");
    CHECK(!L::system_locale().empty());
}
static void region() {
    CHECK(L::first_day_of_week("en-US") == L::weekday::sun);
    CHECK(L::first_day_of_week("de") == L::weekday::mon);
    CHECK(L::first_day_of_week("en-GB-u-fw-sun") == L::weekday::sun);
    EQN(L::min_days_in_first_week("de"), 4);
    CHECK(L::weekend("he-IL").first == L::weekday::fri);
    CHECK(L::preferred_hour_cycle("en-US") == L::hour_cycle::h12);
    CHECK(L::preferred_hour_cycle("de") == L::hour_cycle::h23);
    EQ(L::currency_for_region("JP"), "JPY");
    EQ(L::currency_for_region("DE"), "EUR");
}
static void plurals_numbers() {
    CHECK(L::plural_category(1.0, "en") == L::plural::one);
    CHECK(L::plural_category("1.0", "en") == L::plural::other);
    CHECK(L::plural_category(2.0, "en", true) == L::plural::two);   // 2nd
    CHECK(L::plural_category(3.0, "ru") == L::plural::few);
    CHECK(L::plural_category(5.0, "ru") == L::plural::many);
    CHECK(L::plural_category(21.0, "ru") == L::plural::one);
    CHECK(L::plural_category(0.0, "ar") == L::plural::zero);
    CHECK(L::plural_category(2.0, "ar") == L::plural::two);
    CHECK(L::plural_category(1.5, "fr") == L::plural::one);
    CHECK(L::plural_category(5.0, "ja") == L::plural::other);
    EQ(L::format_number(1234567.891, "en"), "1,234,567.891");
    EQ(L::format_number(1234567.891, "de"), "1.234.567,891");
    EQ(L::format_number(1234567.891, "fr"), "1" + NNBSP + "234" + NNBSP + "567,891");
    EQ(L::format_number(1234567.891, "de-CH"), "1\xE2\x80\x99" "234\xE2\x80\x99" "567.891");
    EQ(L::format_number(12345678, "hi"), "1,23,45,678");
    EQ(L::format_number(1234, "es"), "1234");                     // minimumGroupingDigits 2
    EQ(L::format_number(12345, "es"), "12.345");
    EQ(L::format_number(1234.5, "ar-EG"), "\xD9\xA1\xD9\xAC\xD9\xA2\xD9\xA3\xD9\xA4\xD9\xAB\xD9\xA5");
    L::number_options o; o.numbering = "latn";
    EQ(L::format_number(1234.5, "ar-EG", o), "1,234.5");
    o = {}; o.min_fraction = 2; o.max_fraction = 2;
    EQ(L::format_number(2.5, "en", o), "2.50");
    o = {}; o.max_fraction = 0;
    EQ(L::format_number(2.5, "en", o), "2");                       // half-even
    EQ(L::format_number(3.5, "en", o), "4");
    o = {}; o.max_significant = 3;
    EQ(L::format_number(123456, "en", o), "123,000");
    o = {}; o.sign = L::sign_display::always;
    EQ(L::format_number(5, "en", o), "+5");
    EQ(L::format_number(-5, "en"), "-5");
    EQ(L::format_percent(0.256, "en"), "26%");
    EQ(L::format_percent(0.256, "de"), "26" + NBSP + "%");
    EQ(L::format_scientific(12345, "en"), "1.234E4");
    EQ(L::format_compact(1234, "en"), "1.2K");
    EQ(L::format_compact(1500000, "en", L::compact_width::long_), "1.5 million");
    EQ(L::format_compact(12345, "de"), "12.345");
    EQ(L::format_compact(1234567, "ja"), "123\xE4\xB8\x87");
    EQ(L::format_currency(1234.5, "USD", "en-US"), "$1,234.50");
    EQ(L::format_currency(1234.5, "EUR", "de"), "1.234,50" + NBSP + "\xE2\x82\xAC");
    EQ(L::format_currency(1234.5, "JPY", "ja"), "\xEF\xBF\xA5" "1,234");
    EQ(L::format_currency(-5, "USD", "en", {}, true), "($5.00)");
    EQ(L::currency_symbol("USD", "en-CA"), "US$");
    EQ(L::currency_symbol("AUD", "en-AU"), "$");
    EQ(L::currency_name("EUR", "en", 2.0), "euros");
    EQN(L::currency_digits("JPY"), 0);
    EQN(L::currency_digits("KWD"), 3);
    EQ(L::format_bytes(1536, "en"), "1.5 kB");
    auto pn = L::parse_number("1.234,5", "de"); CHECK(pn && pn.value == 1234.5);
    pn = L::parse_number("12%", "en"); CHECK(pn && std::fabs(pn.value - 0.12) < 1e-12);
    pn = L::parse_number("\xD9\xA1\xD9\xA2", "ar-EG"); CHECK(pn && pn.value == 12);
    pn = L::parse_number("abc", "en"); CHECK(!pn);
    EQ(L::localize_digits("2026", "deva"), "\xE0\xA5\xA8\xE0\xA5\xA6\xE0\xA5\xA8\xE0\xA5\xAC");
}
static void lists_units() {
    std::vector<std::string> v{"a", "b", "c"};
    EQ(L::format_list(v, "en"), "a, b, and c");
    EQ(L::format_list(v, "en-GB"), "a, b and c");
    EQ(L::format_list(v, "en", L::list_type::or_), "a, b, or c");
    EQ(L::format_list(std::vector<std::string>{"x", "y"}, "de"), "x und y");
    EQ(L::format_list(std::vector<std::string>{"padre", "hijo"}, "es"), "padre e hijo");
    EQ(L::format_list(std::vector<std::string>{"siete", "ocho"}, "es", L::list_type::or_), "siete u ocho");
    EQ(L::format_unit(5, "length-kilometer", "en", L::unit_width::long_), "5 kilometers");
    EQ(L::format_unit(5, "length-kilometer", "en"), "5 km");
    EQ(L::format_list(std::vector<std::string>{"agua", "hielo"}, "es"), "agua y hielo");
    EQ(L::format_unit(1, "length-kilometer", "en", L::unit_width::short_), "1 km");
    EQ(L::format_unit(3, "duration-hour", "de", L::unit_width::long_), "3 Stunden");
    EQ(L::format_unit(60, "kilometer-per-hour", "en", L::unit_width::short_), "60 km/h");
    EQ(L::unit_display_name("mass-kilogram", "en"), "kilograms");
    auto c = L::convert_unit(100, "celsius", "fahrenheit"); CHECK(c && std::fabs(c.value - 212) < 1e-9);
    c = L::convert_unit(1, "mile", "kilometer"); CHECK(c && std::fabs(c.value - 1.609344) < 1e-12);
    CHECK(!L::convert_unit(1, "mile", "kilogram"));
}
static void calendars() {
    EQN(L::days_from_civil(1970, 1, 1), 0);
    EQN(L::days_from_civil(2000, 3, 1), 11017);
    auto c = L::civil_from_days(19723); EQN(c[0], 2024); EQN(c[1], 1); EQN(c[2], 1);
    auto w = L::to_iso_week(2021, 1, 3); EQN(w.year, 2020); EQN(w.week, 53); EQN(w.weekday, 7);
    w = L::to_iso_week(2026, 10, 31); EQN(w.week, 44); EQN(w.weekday, 6);
    EQN(L::week_of_year(2026, 1, 1, "en-US"), 1);
    EQN(L::week_of_year(2021, 1, 3, "de"), 53);
    auto j = L::from_gregorian(L::calendar::japanese, 2019, 5, 1); EQN(j.year, 1); EQ(L::era_name(L::calendar::japanese, j.era), "Reiwa");
    j = L::from_gregorian(L::calendar::japanese, 1989, 1, 7); EQN(j.year, 64); EQ(L::era_name(L::calendar::japanese, j.era), "Showa");
    EQN(L::from_gregorian(L::calendar::buddhist, 2026, 1, 1).year, 2569);
    EQN(L::from_gregorian(L::calendar::roc, 2026, 1, 1).year, 115);
    auto p = L::from_gregorian(L::calendar::persian, 2024, 3, 20); EQN(p.year, 1403); EQN(p.month, 1); EQN(p.day, 1);
    p = L::from_gregorian(L::calendar::persian, 2026, 10, 31); EQN(p.year, 1405); EQN(p.month, 8); EQN(p.day, 9);
    auto i = L::from_gregorian(L::calendar::islamic_civil, 2024, 3, 11); EQN(i.year, 1445); EQN(i.month, 9); EQN(i.day, 1);
    EQ(L::calendar_month_name(L::calendar::islamic_civil, i), "Ramadan");
    auto h = L::from_gregorian(L::calendar::hebrew, 2025, 9, 23); EQN(h.year, 5786); EQN(h.month, 1); EQN(h.day, 1);
    h = L::from_gregorian(L::calendar::hebrew, 2024, 4, 23); EQN(h.year, 5784); EQ(L::calendar_month_name(L::calendar::hebrew, h), "Nisan"); EQN(h.day, 15);
    EQN(L::months_in_year(L::calendar::hebrew, 5784), 13);
    EQN(L::months_in_year(L::calendar::hebrew, 5785), 12);
    for (auto cal : {L::calendar::hebrew, L::calendar::persian, L::calendar::islamic_civil, L::calendar::islamic_tbla, L::calendar::japanese, L::calendar::roc}) {
        bool ok = true;
        for (std::int64_t d = -40000; d < 40000; d += 37) {
            auto g = L::civil_from_days(d); auto x = L::from_gregorian(cal, g[0], g[1], g[2]); auto back = L::to_gregorian(cal, x);
            if (!back || *back != g) { ok = false; std::printf("  roundtrip %d: %d-%d-%d -> %d/%d/%d\n", int(cal), g[0], g[1], g[2], x.year, x.month, x.day); break; }
        }
        CHECK(ok);
    }
}
static L::datetime dt(int y, int mo, int d, int h, int mi, int s = 0) { L::datetime x; x.year = y; x.month = mo; x.day = d; x.hour = h; x.minute = mi; x.second = s; return x; }
static void dates() {
    auto d = dt(2026, 10, 31, 15, 4, 5);
    EQN(d.weekday(), 6); EQN(d.day_of_year(), 304);
    EQ(L::format_date(d, "en-US", L::date_style::full), "Saturday, October 31, 2026");
    EQ(L::format_date(d, "en-US", L::date_style::short_), "10/31/26");
    EQ(L::format_date(d, "en-GB", L::date_style::short_), "31/10/2026");
    EQ(L::format_date(d, "de", L::date_style::long_), "31. Oktober 2026");
    EQ(L::format_date(d, "ja", L::date_style::full), "2026\xE5\xB9\xB4" "10\xE6\x9C\x88" "31\xE6\x97\xA5\xE5\x9C\x9F\xE6\x9B\x9C\xE6\x97\xA5");
    EQ(L::format_time(d, "en-US"), "3:04" + NNBSP + "PM");
    EQ(L::format_time(d, "en-US-u-hc-h23"), "15:04");
    EQ(L::format_time(d, "de"), "15:04");
    EQ(L::format_datetime(d, "en-US", L::date_style::medium, L::date_style::short_), "Oct 31, 2026, 3:04" + NNBSP + "PM");
    EQ(L::format_pattern(d, "yyyy-MM-dd'T'HH:mm:ss 'o''clock' EEE", "en"), "2026-10-31T15:04:05 o'clock Sat");
    EQ(L::format_pattern(d, "QQQ G w", "en"), "Q4 AD 44");
    EQ(L::format_pattern(d, "d MMMM y", "ar-EG"), "\xD9\xA3\xD9\xA1 \xD8\xA3\xD9\x83\xD8\xAA\xD9\x88\xD8\xA8\xD8\xB1 \xD9\xA2\xD9\xA0\xD9\xA2\xD9\xA6");
    EQ(L::format_pattern(d, "y G", "ja-JP-u-ca-japanese"), "8 Reiwa");
    EQ(L::pattern_for_skeleton("yMMMd", "en"), "MMM d, y");
    EQ(L::pattern_for_skeleton("yMMMMd", "en"), "MMMM d, y");
    EQ(L::pattern_for_skeleton("jm", "en-US"), "h:mm" + NNBSP + "a");
    EQ(L::pattern_for_skeleton("jm", "de"), "HH:mm");
    EQ(L::format_skeleton(d, "MMMEd", "fr"), "sam. 31 oct.");
    auto p = L::parse_pattern("31/10/2026 15:04", "dd/MM/yyyy HH:mm", "en");
    CHECK(p && p.value.year == 2026 && p.value.month == 10 && p.value.day == 31 && p.value.hour == 15);
    p = L::parse_pattern("Oct 31, 2026, 3:04 PM", "MMM d, y, h:mm a", "en");
    CHECK(p && p.value.hour == 15 && p.value.minute == 4 && p.value.month == 10);
    p = L::parse_pattern("31. Oktober 2026", "d. MMMM y", "de"); CHECK(p && p.value.month == 10);
    p = L::parse_pattern("31/13/2026", "dd/MM/yyyy", "en"); CHECK(!p);
    auto iso = L::parse_iso8601("2026-10-31T15:04:05.250+11:00");
    CHECK(iso && iso.value.utc_offset == 39600 && iso.value.nanosecond == 250000000);
    EQ(L::format_iso8601(iso.value), "2026-10-31T15:04:05.250+11:00");
    iso = L::parse_iso8601("2026-W44-6"); CHECK(iso && iso.value.month == 10 && iso.value.day == 31);
    iso = L::parse_iso8601("2026-304"); CHECK(iso && iso.value.day == 31);
    iso = L::parse_iso8601("20261031T150405Z"); CHECK(iso && iso.value.hour == 15 && iso.value.zone == "UTC");
    CHECK(!L::parse_iso8601("2026-02-30"));
    EQ(L::month_name(3, "fr"), "mars");
    EQ(L::weekday_name(0, "de"), "Sonntag");
    EQ(L::day_period(9, "en"), "AM");
    EQ(L::format_relative(-1, L::rel_unit::day, "en"), "yesterday");
    EQ(L::format_relative(-1, L::rel_unit::day, "en", L::rel_width::long_, true), "1 day ago");
    EQ(L::format_relative(3, L::rel_unit::hour, "en"), "in 3 hours");
    EQ(L::format_relative(2, L::rel_unit::day, "de"), "\xC3\xBC" "bermorgen");
    EQ(L::format_relative(5, L::rel_unit::minute, "ru"), "\xD1\x87\xD0\xB5\xD1\x80\xD0\xB5\xD0\xB7 5 \xD0\xBC\xD0\xB8\xD0\xBD\xD1\x83\xD1\x82");
    auto now = L::sys_seconds(std::chrono::seconds(1793000000));
    EQ(L::format_relative_time(now - std::chrono::seconds(7200), now, "en"), "2 hours ago");
    EQ(L::format_relative_time(now + std::chrono::seconds(10), now, "en"), "now");
    EQ(L::format_duration(std::chrono::seconds(3725), "en"), "1 hour, 2 minutes");
    EQ(L::format_duration(std::chrono::seconds(3725), "en", L::unit_width::narrow, 3), "1h 2m 5s");
}
// MSVC's std::chrono::tzdb takes abbreviations from Windows ICU, which gives "GMT-5" where the IANA data says "EST"
// (whether a zone gets a letter abbreviation depends on the user's display locale).
static bool abbrev_is(const std::string& got, const char* iana, const char* gmt_form) {
    if (L::tz_backend() == "windows-registry") return got == std::string(gmt_form).substr(3) || got == "+" + std::string(gmt_form).substr(4) || (got.size() == 3 && got.substr(1) == "0" + std::string(gmt_form).substr(4));   // "+11", "-05"
    return got == iana || (L::tz_backend() == "chrono-tzdb" && got == gmt_form);
}
static void zones() {
    std::printf("  tz backend: %s, local zone: %s\n", L::tz_backend().c_str(), L::local_zone().c_str());
    auto u = L::zone_at("UTC", L::sys_seconds{}); CHECK(u && u.value.offset.count() == 0);
    if (L::tz_backend() == "utc-only") return;
    auto t = L::sys_seconds(std::chrono::seconds(1793000000));   // 2026-10-26 07:33 UTC (AEDT in Sydney)
    auto z = L::zone_at("Australia/Sydney", t); CHECK(z && z.value.offset.count() == 39600 && z.value.dst && abbrev_is(z.value.abbrev, "AEDT", "GMT+11"));
    z = L::zone_at("Australia/Sydney", L::sys_seconds(std::chrono::seconds(1780000000)));   // May 2026: AEST
    CHECK(z && z.value.offset.count() == 36000 && !z.value.dst);
    z = L::zone_at("America/New_York", L::sys_seconds(std::chrono::seconds(4102444800)));  // 2100-01-01: POSIX footer
    CHECK(z && z.value.offset.count() == -18000 && abbrev_is(z.value.abbrev, "EST", "GMT-5"));
    z = L::zone_at("America/New_York", L::sys_seconds(std::chrono::seconds(4118083200)));  // 2100-07-01
    CHECK(z && z.value.offset.count() == -14400);
    CHECK(!L::zone_at("Nowhere/Atlantis", t));
    auto pz = L::zone_at("EST5EDT,M3.2.0,M11.1.0", L::sys_seconds(std::chrono::seconds(1783000000)));
    CHECK(pz && pz.value.offset.count() == -14400);
    // DST gap and overlap in Sydney: 2026-10-04 02:30 doesn't exist, 2026-04-05 02:30 happens twice.
    auto ls = [](int y, int mo, int d, int h, int mi) { return L::days_from_civil(y, mo, d) * 86400 + h * 3600 + mi * 60; };
    auto g = L::from_local("Australia/Sydney", ls(2026, 10, 4, 2, 30)); CHECK(g && g.value.time_since_epoch().count() == ls(2026, 10, 3, 16, 30));
    auto a = L::from_local("Australia/Sydney", ls(2026, 4, 5, 2, 30), true), b = L::from_local("Australia/Sydney", ls(2026, 4, 5, 2, 30), false);
    CHECK(a && b && b.value - a.value == std::chrono::seconds(3600));
    auto r = L::from_time(std::chrono::system_clock::time_point(std::chrono::seconds(1793000000)), "Australia/Sydney");
    CHECK(r && r.value.hour == 18 && r.value.minute == 33 && abbrev_is(r.value.zone_abbrev, "AEDT", "GMT+11"));
    // Windows' ICU-backed tzdb only knows numeric abbreviations (GMT+11), like abbrev_is above.
    if (r) { auto hm = L::format_pattern(r.value, "HH:mm zzz xxx", "en");
             CHECK(hm == "18:33 AEDT +11:00" || hm == "18:33 GMT+11 +11:00" || hm == "18:33 +11 +11:00"); EQ(L::format_pattern(r.value, "VV O", "en"), "Australia/Sydney GMT+11"); }
    auto names = L::zone_names(); CHECK(names.size() > (L::tz_backend() == "windows-registry" ? 100u : 300u));
}
static void set_env(const char* k, const char* v) {   // v = nullptr unsets
#if defined(_WIN32)
    _putenv_s(k, v ? v : "");
#else
    if (v) ::setenv(k, v, 1); else ::unsetenv(k);
#endif
}
static std::string be32(std::uint32_t v) { return {char(v >> 24), char(v >> 16), char(v >> 8), char(v)}; }
static std::string nul(std::size_t n = 1) { return std::string(n, '\0'); }
// TZif v1 with one fixed type (no transitions).
static std::string tzif_fixed(int off, const std::string& abbr) {
    std::string h = "TZif" + nul(16);
    h += be32(0) + be32(0) + be32(0) + be32(0) + be32(1) + be32(std::uint32_t(abbr.size() + 1));
    return h + be32(std::uint32_t(off)) + nul() + nul() + abbr + nul();
}
static void zone_safety() {
    // Zone names are IANA-style; paths, traversal and devices never reach the file system.
    for (const char* bad : {"../../../../../../dev/zero", "/dev/zero", "/etc/passwd", "Europe/../../etc/passwd", "a//b", "/"})
        CHECK(!L::zone_at(bad, L::sys_seconds{}));
    auto tmp = std::filesystem::temp_directory_path() / "forge_test_locale_tz";
    std::filesystem::remove_all(tmp); std::filesystem::create_directories(tmp);
    auto zf = tmp / "Custom";
    { std::ofstream f(zf, std::ios::binary); auto z = tzif_fixed(3 * 3600 + 1800, "XYZ"); f.write(z.data(), std::streamsize(z.size())); }
    auto abs = zf.string();
    CHECK(!L::zone_at(abs, L::sys_seconds{}));                    // absolute: only the path $TZ names
    const char* old_tz = std::getenv("TZ"); std::string saved = old_tz ? old_tz : ""; bool had_tz = old_tz != nullptr;
    set_env("TZ", (":" + abs).c_str());
    auto z = L::zone_at("", L::sys_seconds{}); CHECK(z && z.value.offset.count() == 12600 && z.value.abbrev == "XYZ");
    set_env("TZ", had_tz ? saved.c_str() : nullptr);
    // Android's tzdata bundle, found through $TZDIR/tzdata.
    std::string a = tzif_fixed(-7 * 3600, "AAA"), b = tzif_fixed(5 * 3600 + 2700, "BBB");
    auto entry = [](std::string name, std::uint32_t off, std::uint32_t len) { name.resize(40, '\0'); return name + be32(off) + be32(len) + be32(0); };
    std::string idx = entry("Forge/Alpha", 0, std::uint32_t(a.size())) + entry("Forge/Beta", std::uint32_t(a.size()), std::uint32_t(b.size()));
    std::string bundle = "tzdata2099z" + nul() + be32(24) + be32(24 + std::uint32_t(idx.size())) + be32(24 + std::uint32_t(idx.size() + a.size() + b.size()));
    bundle += idx + a + b;
    { std::ofstream f(tmp / "tzdata", std::ios::binary); f.write(bundle.data(), std::streamsize(bundle.size())); }
    const char* old_dir = std::getenv("TZDIR"); std::string saved_dir = old_dir ? old_dir : ""; bool had_dir = old_dir != nullptr;
    set_env("TZDIR", tmp.string().c_str());
    auto zb = L::zone_at("Forge/Beta", L::sys_seconds{}); CHECK(zb && zb.value.offset.count() == 20700 && zb.value.abbrev == "BBB");
    auto za = L::zone_at("Forge/Alpha", L::sys_seconds{}); CHECK(za && za.value.offset.count() == -25200);
    CHECK(!L::zone_at("Forge/Gamma", L::sys_seconds{}));
    set_env("TZDIR", had_dir ? saved_dir.c_str() : nullptr);
    auto eg = L::zone_at("Etc/GMT+5", L::sys_seconds{}); CHECK(eg && eg.value.offset.count() == -18000);
#if !defined(_WIN32)
    auto fifo = tmp / "fifo";   // a FIFO named by $TZ must not block the read
    if (::mkfifo(fifo.c_str(), 0600) == 0) {
        set_env("TZ", fifo.c_str());
        CHECK(!L::zone_at("", L::sys_seconds{}));
        set_env("TZ", had_tz ? saved.c_str() : nullptr);
    }
#endif
    std::filesystem::remove_all(tmp);
}
static void categories() {
    auto h = L::current(L::category::time);   // whatever the OS says, it is a usable tag
    CHECK(!h.empty() && !L::current(L::category::numeric).empty());
    std::printf("  locale: messages %s, time %s, numeric %s\n", L::current().c_str(), h.c_str(), L::current(L::category::numeric).c_str());
#if !defined(_WIN32)
    const char* names[] = {"LC_ALL", "LC_MESSAGES", "LC_TIME", "LC_NUMERIC", "LANG"};
    std::vector<std::pair<bool, std::string>> saved;
    for (auto n : names) { const char* v = std::getenv(n); saved.push_back({v != nullptr, v ? v : ""}); }
    set_env("LC_ALL", nullptr); set_env("LC_MESSAGES", nullptr);
    set_env("LANG", "en_US.UTF-8"); set_env("LC_TIME", "de_DE.UTF-8"); set_env("LC_NUMERIC", "fr_FR.UTF-8");
    EQ(L::system_locale(), "en-US"); EQ(L::current(), "en-US");
    EQ(L::current(L::category::time), "de-DE"); EQ(L::current(L::category::numeric), "fr-FR");
    EQ(L::format_number(1234.5), "1" + NNBSP + "234,5");                             // LC_NUMERIC
    L::datetime d; d.year = 2026; d.month = 3; d.day = 9;
    EQ(L::format_date(d, "", L::date_style::long_), L::format_date(d, "de-DE", L::date_style::long_));   // LC_TIME
    CHECK(L::first_day_of_week() == L::weekday::mon);
    set_env("LC_ALL", "ja_JP.UTF-8");                                                 // LC_ALL wins everywhere
    EQ(L::current(L::category::time), "ja-JP"); EQ(L::current(L::category::numeric), "ja-JP");
    L::set_current("pt-BR"); EQ(L::current(L::category::time), "pt-BR"); L::set_current("");
    for (std::size_t i = 0; i < saved.size(); ++i) set_env(names[i], saved[i].first ? saved[i].second.c_str() : nullptr);
#endif
}
static void messages() {
    auto m = L::format_message("{n, plural, =0 {no files} one {# file} other {# files}} in {dir}", {{"n", 1234}, {"dir", "/tmp"}}, "en");
    CHECK(m); EQ(m.value, "1,234 files in /tmp");
    m = L::format_message("{n, plural, =0 {no files} one {# file} other {# files}}", {{"n", 0}}, "en"); EQ(m.value, "no files");
    m = L::format_message("{n, plural, one {# \xD1\x84\xD0\xB0\xD0\xB9\xD0\xBB} few {# \xD1\x84\xD0\xB0\xD0\xB9\xD0\xBB\xD0\xB0} many {# \xD1\x84\xD0\xB0\xD0\xB9\xD0\xBB\xD0\xBE\xD0\xB2} other {# \xD1\x84\xD0\xB0\xD0\xB9\xD0\xBB\xD0\xB0}}", {{"n", 3}}, "ru");
    EQ(m.value, "3 \xD1\x84\xD0\xB0\xD0\xB9\xD0\xBB\xD0\xB0");
    m = L::format_message("{g, select, female {She} male {He} other {They}} replied", {{"g", "female"}}, "en"); EQ(m.value, "She replied");
    m = L::format_message("{p, selectordinal, one {#st} two {#nd} few {#rd} other {#th}}", {{"p", 23}}, "en"); EQ(m.value, "23rd");
    m = L::format_message("{n, plural, offset:1 =0 {nobody} =1 {just {who}} one {{who} and # other} other {{who} and # others}}", {{"n", 3}, {"who", "Ann"}}, "en");
    EQ(m.value, "Ann and 2 others");
    m = L::format_message("It''s '{literal}' {x, number, percent}", {{"x", 0.5}}, "en"); EQ(m.value, "It's {literal} 50%");
    m = L::format_message("{d, date, long}", {{"d", dt(2026, 10, 31, 0, 0)}}, "en"); EQ(m.value, "October 31, 2026");
    CHECK(!L::format_message("{n, plural, one {x}}", {{"n", 5}}, "en"));
    CHECK(!L::format_message("{unclosed", {}, "en"));
}
static void gettext() {
    auto e = L::eval_plural_expr("n%10==1 && n%100!=11 ? 0 : n%10>=2 && n%10<=4 && (n%100<10 || n%100>=20) ? 1 : 2", 22);
    CHECK(e && e.value == 1);
    CHECK(!L::eval_plural_expr("n +", 1));
    CHECK(!L::eval_plural_expr("n / 0", 1));
    CHECK(!L::eval_plural_expr(std::string(200000, '!') + "n", 1));    // deep '!' chains fail instead of overflowing the stack
    CHECK(!L::eval_plural_expr(std::string(200000, '(') + "n", 1));
    auto nn = L::eval_plural_expr("!!n", 5); CHECK(nn && nn.value == 1);
    const char* po =
        "# comment\nmsgid \"\"\nmsgstr \"\"\n\"Language: ru\\n\"\n\"Plural-Forms: nplurals=3; plural=(n%10==1 && n%100!=11 ? 0 : n%10>=2 && n%10<=4 && (n%100<10 || n%100>=20) ? 1 : 2);\\n\"\n\n"
        "msgid \"Hello\"\nmsgstr \"\xD0\x9F\xD1\x80\xD0\xB8\xD0\xB2\xD0\xB5\xD1\x82\"\n\n"
        "msgctxt \"menu\"\nmsgid \"Open\"\nmsgstr \"\xD0\x9E\xD1\x82\xD0\xBA\xD1\x80\xD1\x8B\xD1\x82\xD1\x8C\"\n\n"
        "msgid \"%d file\"\nmsgid_plural \"%d files\"\nmsgstr[0] \"%d \xD1\x84\xD0\xB0\xD0\xB9\xD0\xBB\"\nmsgstr[1] \"%d \xD1\x84\xD0\xB0\xD0\xB9\xD0\xBB\xD0\xB0\"\nmsgstr[2] \"%d \xD1\x84\xD0\xB0\xD0\xB9\xD0\xBB\xD0\xBE\xD0\xB2\"\n\n"
        "#, fuzzy\nmsgid \"Fuzzy\"\nmsgstr \"nope\"\n\n"
        "msgid \"multi\"\nmsgstr \"\"\n\"line\\none\"\n";
    auto c = L::catalog::parse_po(po);
    CHECK(c);
    if (!c) return;
    EQN(c.value.size(), 4u);
    EQ(std::string(c.value.gettext("Hello")), "\xD0\x9F\xD1\x80\xD0\xB8\xD0\xB2\xD0\xB5\xD1\x82");
    EQ(std::string(c.value.gettext("Missing")), "Missing");
    EQ(std::string(c.value.gettext("Fuzzy")), "Fuzzy");
    EQ(std::string(c.value.pgettext("menu", "Open")), "\xD0\x9E\xD1\x82\xD0\xBA\xD1\x80\xD1\x8B\xD1\x82\xD1\x8C");
    EQ(std::string(c.value.gettext("Open")), "Open");
    EQ(std::string(c.value.ngettext("%d file", "%d files", 5)), "%d \xD1\x84\xD0\xB0\xD0\xB9\xD0\xBB\xD0\xBE\xD0\xB2");
    EQ(std::string(c.value.ngettext("%d file", "%d files", 21)), "%d \xD1\x84\xD0\xB0\xD0\xB9\xD0\xBB");
    EQ(std::string(c.value.gettext("multi")), "line\none");
    EQ(c.value.header("Language"), "ru");
    // Build a little-endian .mo by hand and read it back from a directory tree.
    auto mo = [](bool be) {
        std::vector<std::pair<std::string, std::string>> e{{"", "Plural-Forms: nplurals=2; plural=n != 1;\n"}, {"Hello", "Hallo"}, {"cat\0cats", std::string("Katze\0Katzen", 12)}};
        e[2].first = std::string("cat\0cats", 8);
        std::string hdr(28, '\0'), tabs, strs; std::uint32_t n = 3, o = 28, t = 28 + 8 * n, base = 28 + 16 * n;
        auto put = [&](std::string& s, std::size_t at, std::uint32_t v) { for (int i = 0; i < 4; ++i) s[at + i] = char(be ? v >> (24 - 8 * i) : v >> (8 * i)); };
        put(hdr, 0, 0x950412de); put(hdr, 8, n); put(hdr, 12, o); put(hdr, 16, t);
        std::string ot(8 * n, '\0'), tt(8 * n, '\0'); std::uint32_t pos = base;
        for (std::uint32_t i = 0; i < n; ++i) { put(ot, 8 * i, std::uint32_t(e[i].first.size())); put(ot, 8 * i + 4, pos); strs += e[i].first + '\0'; pos += std::uint32_t(e[i].first.size()) + 1; }
        for (std::uint32_t i = 0; i < n; ++i) { put(tt, 8 * i, std::uint32_t(e[i].second.size())); put(tt, 8 * i + 4, pos); strs += e[i].second + '\0'; pos += std::uint32_t(e[i].second.size()) + 1; }
        return hdr + ot + tt + strs;
    };
    for (bool be : {false, true}) {
        auto m = L::catalog::parse_mo(mo(be));
        CHECK(m);
        if (m) { EQ(std::string(m.value.gettext("Hello")), "Hallo"); EQ(std::string(m.value.ngettext("cat", "cats", 2)), "Katzen"); }
    }
    CHECK(!L::catalog::parse_mo("garbage"));
    auto dir = std::filesystem::temp_directory_path() / "forge_test_locale_mo";
    std::filesystem::create_directories(dir / "de" / "LC_MESSAGES");
    { std::ofstream f(dir / "de" / "LC_MESSAGES" / "app.mo", std::ios::binary); auto s = mo(false); f.write(s.data(), std::streamsize(s.size())); }
    auto fc = L::catalog::find(dir.string(), "app", "de-AT");
    CHECK(fc && std::string(fc.value.gettext("Hello")) == "Hallo");
    CHECK(!L::catalog::find(dir.string(), "app", "fr"));
    std::filesystem::remove_all(dir);
}
int main() {
    tags(); region(); plurals_numbers(); lists_units(); calendars(); dates(); zones(); zone_safety(); categories(); messages(); gettext();
    std::printf("test_locale: %d/%d checks passed\n", checks - fails, checks);
    return fails ? 1 : 0;
}
