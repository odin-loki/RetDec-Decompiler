// forge::os::locale: OS locale detection, BCP 47 tags and CLDR-based formatting without ICU.
// Numbers, currency, percent, compact, dates/times (LDML patterns, styles, skeletons), relative time, durations,
// plurals (every CLDR language), lists, units, time zones (TZif / std::chrono tzdb), calendars,
// region data, MessageFormat-lite and gettext catalogs. Bundled data: CLDR 46 for 52 locales (docs/text-locale.md).
#pragma once
#include <forge/os/os.hpp>
#include <array>
#include <chrono>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace forge::os::locale {

// ---------------------------------------------------------------- tags and detection
struct tag {
    std::string language, script, region;
    std::vector<std::string> variants;
    std::vector<std::pair<std::string, std::string>> unicode;   // -u- keywords: {"co","phonebk"}, {"nu","arab"}, {"hc","h23"}
    std::string str() const;                                    // canonical BCP 47
    std::string_view keyword(std::string_view k) const;
};
// Lenient: accepts POSIX ("en_AU.UTF-8@euro"), Windows ("en-AU") and Apple ("zh-Hans_CN") forms; "C"/"POSIX" map to "en".
tag parse_tag(std::string_view s);
std::string canonicalize(std::string_view s);                    // "EN_au.utf8" -> "en-AU"
tag maximize(const tag& t);                                     // likely subtags: "zh-TW" -> zh-Hant-TW
tag minimize(const tag& t);                                     // "en-Latn-US" -> en
std::vector<std::string> fallback_chain(std::string_view locale); // "zh-Hant-TW" -> zh-Hant-TW, zh-Hant, zh, root
std::string best_bundled(std::string_view locale);              // nearest bundled data: "en-NZ" -> "en-GB", "es-AR" -> "es-MX", "zh-TW" -> "zh-Hant"
std::vector<std::string> bundled_locales();
std::string negotiate(std::span<const std::string> requested, std::span<const std::string> available, std::string_view fallback = "en");

std::string system_locale();                                    // OS user locale, canonical tag; "en" if unknown
// Formatting category. POSIX: LC_ALL, then LC_MESSAGES / LC_TIME / LC_NUMERIC, then LANG. Everywhere, the time
// category carries the user's OS overrides (Windows Region settings, Apple Language & Region: 24-hour clock, first
// weekday) as -u-hc / -u-fw keywords; number formatting with the default locale uses the user's separators.
enum class category { messages, time, numeric };
std::string system_locale(category c);
std::string current(category c);                                // set_current() override, else system_locale(c)
std::vector<std::string> preferred_languages();                 // OS UI language list, most preferred first
std::string current();                                          // set_current() override, else system_locale()
void set_current(std::string_view locale);                      // process-wide default for "" locale arguments
void android_set_locale(std::string_view locale, std::span<const std::string> preferred = {});   // from JNI glue
std::string display_name(std::string_view language, std::string_view in_locale = {});           // "de" in fr -> "allemand"

// ---------------------------------------------------------------- region data
enum class weekday : std::uint8_t { sun, mon, tue, wed, thu, fri, sat };
weekday first_day_of_week(std::string_view locale = {});
int min_days_in_first_week(std::string_view locale = {});
std::pair<weekday, weekday> weekend(std::string_view locale = {});
enum class measurement { metric, us, uk };
measurement measurement_system(std::string_view locale = {});
std::string paper_size(std::string_view locale = {});           // "A4" / "US-Letter"
enum class hour_cycle { h11, h12, h23, h24 };
hour_cycle preferred_hour_cycle(std::string_view locale = {});
std::string currency_for_region(std::string_view region_or_locale);   // "AU" / "en-AU" -> "AUD"

// ---------------------------------------------------------------- plurals
enum class plural : std::uint8_t { zero, one, two, few, many, other };
const char* plural_name(plural p);
// CLDR operands from the decimal string (keeps visible fraction digits: "1.0" is not "one" in English).
plural plural_category(std::string_view number, std::string_view locale = {}, bool ordinal = false);
plural plural_category(double n, std::string_view locale = {}, bool ordinal = false);
std::vector<plural> plural_categories(std::string_view locale = {}, bool ordinal = false);

// ---------------------------------------------------------------- numbers
enum class sign_display { auto_, always, never, except_zero };
enum class rounding { half_even, half_up, half_down, ceiling, floor, down, up };
struct number_options {
    int min_integer = 1;
    int min_fraction = -1, max_fraction = -1;    // -1: from the locale pattern / currency digits
    int max_significant = 0;                     // >0 switches to significant digits
    bool grouping = true;
    sign_display sign = sign_display::auto_;
    rounding round = rounding::half_even;
    std::string numbering;                       // "" locale default, "latn", "native", "arab", "deva", "thai", ...
};
std::string format_number(double v, std::string_view locale = {}, const number_options& o = {});
std::string format_integer(std::int64_t v, std::string_view locale = {}, const number_options& o = {});
std::string format_percent(double fraction, std::string_view locale = {}, const number_options& o = {});   // 0.25 -> "25%"
std::string format_permille(double fraction, std::string_view locale = {}, const number_options& o = {});
std::string format_scientific(double v, std::string_view locale = {}, int max_fraction = 3);
enum class compact_width { short_, long_ };
std::string format_compact(double v, std::string_view locale = {}, compact_width w = compact_width::short_, int max_significant = 2);  // 1.2K, 1.2 thousand
enum class currency_display { symbol, narrow_symbol, code, name };
std::string format_currency(double amount, std::string_view currency, std::string_view locale = {},
                            currency_display d = currency_display::symbol, bool accounting = false, const number_options& o = {});
std::string format_currency_compact(double amount, std::string_view currency, std::string_view locale = {});
std::string format_bytes(std::uint64_t bytes, std::string_view locale = {}, bool binary = false);           // "1.5 MB", "1.4 MiB"
// Parsing accepts the locale's symbols and digits, grouping, percent/permille, exponent, and ASCII fallbacks.
result<double> parse_number(std::string_view s, std::string_view locale = {});
struct currency_amount { double amount = 0; std::string currency; };
result<currency_amount> parse_currency(std::string_view s, std::string_view locale = {});
std::string currency_symbol(std::string_view currency, std::string_view locale = {}, bool narrow = false);
std::string currency_name(std::string_view currency, std::string_view locale = {}, std::optional<double> count = {});
int currency_digits(std::string_view currency, bool cash = false);
std::string number_symbol(std::string_view name, std::string_view locale = {});   // "decimal", "group", "minusSign", ...
std::string localize_digits(std::string_view ascii, std::string_view numbering);  // "123" + "arab" -> "١٢٣"

// ---------------------------------------------------------------- lists and units
enum class list_type { and_, or_, unit };
enum class list_width { wide, short_, narrow };
std::string format_list(std::span<const std::string> items, std::string_view locale = {}, list_type t = list_type::and_, list_width w = list_width::wide);
enum class unit_width { long_, short_, narrow };
// unit: CLDR id with or without category ("kilometer-per-hour", "length-meter", "megabyte"). Compound "x-per-y" units
// without data use the locale's per pattern.
std::string format_unit(double v, std::string_view unit, std::string_view locale = {}, unit_width w = unit_width::short_, const number_options& o = {});
std::string unit_display_name(std::string_view unit, std::string_view locale = {}, unit_width w = unit_width::long_);
result<double> convert_unit(double v, std::string_view from, std::string_view to);   // length, mass, temperature, volume, speed, area, duration, digital, pressure, energy

// ---------------------------------------------------------------- time zones
using sys_seconds = std::chrono::sys_seconds;
struct zone_info { std::chrono::seconds offset{0}; bool dst = false; std::string abbrev; };
std::string local_zone();                                       // IANA name ("Australia/Sydney"), "UTC" if unknown
std::vector<std::string> zone_names();
result<zone_info> zone_at(std::string_view zone, sys_seconds t);   // zone "" = local
result<sys_seconds> from_local(std::string_view zone, std::int64_t local_seconds_since_epoch, bool earlier = true);
std::string tz_backend();                                       // "chrono-tzdb", "tzif:<dir>", or "utc-only"

// ---------------------------------------------------------------- dates and times
struct datetime {
    int year = 1970, month = 1, day = 1, hour = 0, minute = 0, second = 0;
    int nanosecond = 0;
    int utc_offset = 0;                 // seconds east of UTC
    std::string zone, zone_abbrev;      // filled by from_time
    int weekday() const;                // 0 = Sunday
    int day_of_year() const;            // 1-based
    std::int64_t local_seconds() const; // seconds since 1970-01-01T00:00 local
    sys_seconds to_sys() const;         // local_seconds - utc_offset
};
result<datetime> from_time(std::chrono::system_clock::time_point t, std::string_view zone = {});   // zone "" = local, "UTC"
datetime from_time_utc(std::chrono::system_clock::time_point t);
datetime now(std::string_view zone = {});
enum class date_style { none, full, long_, medium, short_ };
std::string format_date(const datetime& d, std::string_view locale = {}, date_style s = date_style::medium);
std::string format_time(const datetime& d, std::string_view locale = {}, date_style s = date_style::short_);
std::string format_datetime(const datetime& d, std::string_view locale = {}, date_style date = date_style::medium, date_style time = date_style::short_);
// LDML pattern: G y yy yyyy u Q M MM MMM MMMM MMMMM L d D E EEEE EEEEE c e a h H k K m s S... z zzzz Z ZZZZZ x X O w 'quoted'.
std::string format_pattern(const datetime& d, std::string_view pattern, std::string_view locale = {});
// Skeleton ("yMMMd", "Hm", "MMMMEEEEd") resolved through the locale's availableFormats, best-match when not exact.
std::string format_skeleton(const datetime& d, std::string_view skeleton, std::string_view locale = {});
std::string pattern_for_skeleton(std::string_view skeleton, std::string_view locale = {});
result<datetime> parse_pattern(std::string_view s, std::string_view pattern, std::string_view locale = {});
result<datetime> parse_iso8601(std::string_view s);            // date, date-time, offsets, Z, fractional seconds, week dates
std::string format_iso8601(const datetime& d, bool with_offset = true);
std::string month_name(int month, std::string_view locale = {}, std::string_view width = "wide", bool standalone = false);
std::string weekday_name(int weekday, std::string_view locale = {}, std::string_view width = "wide", bool standalone = false);
std::string day_period(int hour, std::string_view locale = {}, std::string_view width = "abbreviated");

enum class rel_unit { second, minute, hour, day, week, month, quarter, year };
enum class rel_width { long_, short_, narrow };
// numeric=false allows "yesterday"/"next week"; true always gives "in 1 day".
std::string format_relative(double value, rel_unit u, std::string_view locale = {}, rel_width w = rel_width::long_, bool numeric = false);
std::string format_relative_time(sys_seconds target, sys_seconds now, std::string_view locale = {}, rel_width w = rel_width::long_);
std::string format_duration(std::chrono::seconds d, std::string_view locale = {}, unit_width w = unit_width::long_, int max_units = 2);

// ---------------------------------------------------------------- calendars
enum class calendar { gregorian, buddhist, roc, japanese, islamic_civil, islamic_tbla, hebrew, persian, iso8601 };
struct cal_date { int year = 0, month = 1, day = 1; int era = 0; bool leap_month = false; };
std::optional<calendar> calendar_from_name(std::string_view name);   // "islamic-civil", "japanese", "persian"...
cal_date from_gregorian(calendar c, int y, int m, int d);
std::optional<std::array<int, 3>> to_gregorian(calendar c, const cal_date& d);
int months_in_year(calendar c, int year);
int days_in_month(calendar c, int year, int month);
std::string calendar_month_name(calendar c, const cal_date& d);      // transliterated English names ("Ramadan", "Nisan", "Farvardin")
std::string era_name(calendar c, int era);                           // "Reiwa", "BE", "Minguo", "AH", "AM", "AP"
struct iso_week { int year, week, weekday; };                        // weekday 1 = Monday
iso_week to_iso_week(int y, int m, int d);
int week_of_year(int y, int m, int d, std::string_view locale = {}); // locale first day / min days rules
std::int64_t days_from_civil(int y, int m, int d);                   // days since 1970-01-01, proleptic Gregorian
std::array<int, 3> civil_from_days(std::int64_t z);

// ---------------------------------------------------------------- messages (ICU MessageFormat subset)
struct message_value : std::variant<std::monostate, std::int64_t, double, std::string, datetime> {
    using variant::variant;
    message_value(int v) : variant(std::int64_t(v)) {}
    message_value(const char* s) : variant(std::string(s)) {}
};
using message_args = std::map<std::string, message_value, std::less<>>;
// {name} {n, number[, integer|percent|currency[/CODE]|compact|::skeleton]} {d, date|time[, short|medium|long|full|pattern]}
// {n, plural, [offset:k] =0 {..} one {# item} other {# items}} {n, selectordinal, ...} {g, select, female {..} other {..}}
// Apostrophe quoting: '{' literal, '' -> '.
result<std::string> format_message(std::string_view pattern, const message_args& args, std::string_view locale = {});

// ---------------------------------------------------------------- gettext catalogs
class catalog {
public:
    static result<catalog> load(std::string_view path);              // .mo or .po by content
    static result<catalog> parse_mo(std::string_view bytes);
    static result<catalog> parse_po(std::string_view text);
    // <dir>/<locale>/LC_MESSAGES/<domain>.mo (or .po), tried along the fallback chain of `locale` (POSIX "pt_BR" too).
    static result<catalog> find(std::string_view dir, std::string_view domain, std::string_view locale = {});
    catalog(); ~catalog(); catalog(catalog&&) noexcept; catalog& operator=(catalog&&) noexcept;
    std::string_view gettext(std::string_view msgid) const;
    std::string_view ngettext(std::string_view singular, std::string_view plural_, std::uint64_t n) const;
    std::string_view pgettext(std::string_view ctx, std::string_view msgid) const;
    std::string_view npgettext(std::string_view ctx, std::string_view singular, std::string_view plural_, std::uint64_t n) const;
    std::optional<std::string_view> lookup(std::string_view ctx, std::string_view msgid, std::uint64_t n = 1) const;  // nullopt if untranslated
    std::string header(std::string_view field) const;                 // "Language", "Plural-Forms", ...
    std::size_t size() const;
    unsigned plural_index(std::uint64_t n) const;                     // from Plural-Forms (C expression)
    void merge(catalog&& other);                                      // other's entries fill gaps
private:
    struct impl; std::unique_ptr<impl> p_;
};
// Evaluates a gettext Plural-Forms C expression ("n%10==1 && n%100!=11 ? 0 : ...").
result<std::uint64_t> eval_plural_expr(std::string_view expr, std::uint64_t n);

}  // namespace forge::os::locale
