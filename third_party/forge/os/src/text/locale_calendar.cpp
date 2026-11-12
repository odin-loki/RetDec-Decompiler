// forge::os::locale: civil-day arithmetic and non-Gregorian calendars (arithmetic algorithms, no tables).
#include "locale_internal.hpp"
#include <cmath>

namespace forge::os::locale {
using namespace detail;

std::int64_t days_from_civil(int y, int m, int d) {   // Howard Hinnant's algorithm
    y -= m <= 2;
    std::int64_t era = (y >= 0 ? y : y - 399) / 400;
    unsigned yoe = unsigned(y - era * 400), doy = (153 * unsigned(m > 2 ? m - 3 : m + 9) + 2) / 5 + unsigned(d) - 1, doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + std::int64_t(doe) - 719468;
}
std::array<int, 3> civil_from_days(std::int64_t z) {
    z += 719468;
    std::int64_t era = (z >= 0 ? z : z - 146096) / 146097;
    unsigned doe = unsigned(z - era * 146097), yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    std::int64_t y = std::int64_t(yoe) + era * 400;
    unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100), mp = (5 * doy + 2) / 153, d = doy - (153 * mp + 2) / 5 + 1, m = mp < 10 ? mp + 3 : mp - 9;
    return {int(y + (m <= 2)), int(m), int(d)};
}

namespace {
std::int64_t fdiv(std::int64_t a, std::int64_t b) { return a / b - ((a % b != 0) && ((a < 0) != (b < 0))); }
constexpr std::int64_t unix_rd = 719163;     // R.D. (0001-01-01 = 1) of 1970-01-01
constexpr std::int64_t unix_jdn = 2440588;   // Julian day number of 1970-01-01
bool greg_leap(int y) { return (y % 4 == 0 && y % 100 != 0) || y % 400 == 0; }

// Hebrew (Reingold & Dershowitz). R.D. months: 1 = Nisan ... 7 = Tishri, 12 = Adar (I), 13 = Adar II.
constexpr std::int64_t heb_epoch = -1373427;
bool heb_leap(std::int64_t y) { return ((7 * y + 1) % 19 + 19) % 19 < 7; }
std::int64_t heb_elapsed(std::int64_t y) {
    std::int64_t months = fdiv(235 * y - 234, 19), parts = 12084 + 13753 * months, day = months * 29 + fdiv(parts, 25920);
    return ((3 * (day + 1)) % 7 + 7) % 7 < 3 ? day + 1 : day;
}
int heb_corr(std::int64_t y) {
    auto a = heb_elapsed(y - 1), b = heb_elapsed(y), c = heb_elapsed(y + 1);
    return c - b == 356 ? 2 : b - a == 382 ? 1 : 0;
}
std::int64_t heb_new_year(std::int64_t y) { return heb_epoch + heb_elapsed(y) + heb_corr(y); }
int heb_year_days(std::int64_t y) { return int(heb_new_year(y + 1) - heb_new_year(y)); }
int heb_month_days_rd(std::int64_t y, int m) {
    int yd = heb_year_days(y);
    if (m == 2 || m == 4 || m == 6 || m == 10 || m == 13 || (m == 12 && !heb_leap(y)) || (m == 8 && yd % 10 != 5) || (m == 9 && yd % 10 == 3)) return 29;
    return 30;
}
// Year-order month k (1 = Tishri) -> R.D. month.
int heb_rd_month(std::int64_t y, int k) { int n = heb_leap(y) ? 13 : 12; return k <= n - 6 ? k + 6 : k - (n - 6); }
std::int64_t heb_fixed(std::int64_t y, int k, int d) {
    std::int64_t f = heb_new_year(y);
    for (int i = 1; i < k; ++i) f += heb_month_days_rd(y, heb_rd_month(y, i));
    return f + d - 1;
}

// Persian, 33-year arithmetic cycle (as ICU).
constexpr std::int64_t pers_epoch = 1948320;
std::int64_t pers_first(std::int64_t y) { return 365 * (y - 1) + fdiv(8 * y + 21, 33); }
bool pers_leap(std::int64_t y) { return ((25 * y + 11) % 33 + 33) % 33 < 8; }

// Islamic arithmetic (Type II leap years). civil epoch Friday 622-07-16 (Julian); tbla one day earlier.
std::int64_t isl_epoch(calendar c) { return c == calendar::islamic_tbla ? 1948439 : 1948440; }
std::int64_t isl_month_start(int y, int m) { return std::int64_t(std::ceil(29.5 * (m - 1))) + std::int64_t(y - 1) * 354 + fdiv(3 + 11 * std::int64_t(y), 30); }

struct jera { int y, m, d; const char* name; };
constexpr jera jeras[] = {{1868, 9, 8, "Meiji"}, {1912, 7, 30, "Taisho"}, {1926, 12, 25, "Showa"}, {1989, 1, 8, "Heisei"}, {2019, 5, 1, "Reiwa"}};
}  // namespace

std::optional<calendar> calendar_from_name(std::string_view n) {
    if (n == "gregorian" || n == "gregory") return calendar::gregorian;
    if (n == "buddhist") return calendar::buddhist;
    if (n == "roc") return calendar::roc;
    if (n == "japanese") return calendar::japanese;
    if (n == "islamic-civil" || n == "islamicc" || n == "islamic") return calendar::islamic_civil;
    if (n == "islamic-tbla") return calendar::islamic_tbla;
    if (n == "hebrew") return calendar::hebrew;
    if (n == "persian") return calendar::persian;
    if (n == "iso8601") return calendar::iso8601;
    return std::nullopt;
}

cal_date from_gregorian(calendar c, int y, int m, int d) {
    std::int64_t days = days_from_civil(y, m, d);
    switch (c) {
    case calendar::gregorian: case calendar::iso8601: return {y, m, d, y > 0 ? 1 : 0};
    case calendar::buddhist: return {y + 543, m, d, 0};
    case calendar::roc: return y > 1911 ? cal_date{y - 1911, m, d, 1} : cal_date{1912 - y, m, d, 0};
    case calendar::japanese: {
        int e = -1;
        for (int i = 4; i >= 0; --i) if (days >= days_from_civil(jeras[i].y, jeras[i].m, jeras[i].d)) { e = i; break; }
        if (e < 0) return {y, m, d, -1};
        return {y - jeras[e].y + 1, m, d, e};
    }
    case calendar::islamic_civil: case calendar::islamic_tbla: {
        std::int64_t jd = days + unix_jdn, ep = isl_epoch(c), off = jd - ep;
        int yy = int(fdiv(30 * off + 10646, 10631));
        int mm = int(std::min<std::int64_t>(12, std::int64_t(std::ceil(double(off - 29 - isl_month_start(yy, 1)) / 29.5)) + 1));
        mm = std::max(1, mm);
        while (mm > 1 && off < isl_month_start(yy, mm)) --mm;
        return {yy, mm, int(off - isl_month_start(yy, mm)) + 1, 0};
    }
    case calendar::persian: {
        std::int64_t since = days + unix_jdn - pers_epoch;
        std::int64_t yy = 1 + fdiv(33 * since + 3, 12053);
        std::int64_t doy = since - pers_first(yy);
        if (doy < 0) { --yy; doy = since - pers_first(yy); }
        int mm = doy < 216 ? int(doy / 31) : int((doy - 6) / 30);
        int start = mm < 6 ? mm * 31 : mm * 30 + 6;
        return {int(yy), mm + 1, int(doy - start) + 1, 0};
    }
    case calendar::hebrew: {
        std::int64_t rd = days + unix_rd;
        std::int64_t yy = fdiv((rd - heb_epoch) * 98496, 35975351);
        while (heb_new_year(yy + 1) <= rd) ++yy;
        while (heb_new_year(yy) > rd) --yy;
        int k = 1; std::int64_t f = heb_new_year(yy);
        while (true) { int md = heb_month_days_rd(yy, heb_rd_month(yy, k)); if (rd < f + md) break; f += md; ++k; }
        return {int(yy), k, int(rd - f) + 1, 0, heb_leap(yy) && k == 6};
    }
    }
    return {y, m, d, 1};
}
std::optional<std::array<int, 3>> to_gregorian(calendar c, const cal_date& d) {
    bool greg_like = c == calendar::gregorian || c == calendar::iso8601 || c == calendar::buddhist || c == calendar::roc || c == calendar::japanese;
    if (!greg_like && (d.month < 1 || d.month > months_in_year(c, d.year) || d.day < 1 || d.day > days_in_month(c, d.year, d.month))) return std::nullopt;
    std::int64_t days;
    if (greg_like) {
        int y = c == calendar::buddhist ? d.year - 543 : c == calendar::roc ? (d.era == 1 ? d.year + 1911 : 1912 - d.year)
              : c == calendar::japanese ? (d.era >= 0 && d.era <= 4 ? jeras[d.era].y + d.year - 1 : d.year) : (d.era == 0 && d.year > 0 ? 1 - d.year : d.year);
        if (d.month < 1 || d.month > 12 || d.day < 1 || d.day > days_in_month(calendar::gregorian, y, d.month)) return std::nullopt;
        return std::array{y, d.month, d.day};
    }
    switch (c) {
    case calendar::gregorian: case calendar::iso8601: return std::array{d.era == 0 && d.year > 0 ? 1 - d.year : d.year, d.month, d.day};
    case calendar::buddhist: return std::array{d.year - 543, d.month, d.day};
    case calendar::roc: return std::array{d.era == 1 ? d.year + 1911 : 1912 - d.year, d.month, d.day};
    case calendar::japanese: if (d.era < 0 || d.era > 4) return std::array{d.year, d.month, d.day}; return std::array{jeras[d.era].y + d.year - 1, d.month, d.day};
    case calendar::islamic_civil: case calendar::islamic_tbla: days = isl_epoch(c) + isl_month_start(d.year, d.month) + d.day - 1 - unix_jdn; break;
    case calendar::persian: days = pers_epoch + pers_first(d.year) + (d.month <= 7 ? (d.month - 1) * 31 : (d.month - 1) * 30 + 6) + d.day - 1 - unix_jdn; break;
    case calendar::hebrew: days = heb_fixed(d.year, d.month, d.day) - unix_rd; break;
    default: return std::nullopt;
    }
    return civil_from_days(days);
}
int months_in_year(calendar c, int y) { return c == calendar::hebrew ? (heb_leap(y) ? 13 : 12) : 12; }
int days_in_month(calendar c, int y, int m) {
    switch (c) {
    case calendar::islamic_civil: case calendar::islamic_tbla:
        if (m % 2 == 1) return 30;
        if (m == 12) return ((14 + 11 * y) % 30 + 30) % 30 < 11 ? 30 : 29;
        return 29;
    case calendar::persian: return m <= 6 ? 31 : m <= 11 ? 30 : (pers_leap(y) ? 30 : 29);
    case calendar::hebrew: return heb_month_days_rd(y, heb_rd_month(y, m));
    default: {
        int gy = c == calendar::buddhist ? y - 543 : c == calendar::roc ? y + 1911 : y;
        static const int md[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
        return m == 2 && greg_leap(gy) ? 29 : md[(m - 1) % 12];
    }
    }
}
std::string calendar_month_name(calendar c, const cal_date& d) {
    static const char* isl[] = {"Muharram", "Safar", "Rabi\xCA\xBB I", "Rabi\xCA\xBB II", "Jumada I", "Jumada II", "Rajab", "Sha\xCA\xBB" "ban", "Ramadan", "Shawwal", "Dhu\xCA\xBBl-Qi\xCA\xBB" "dah", "Dhu\xCA\xBBl-Hijjah"};
    static const char* per[] = {"Farvardin", "Ordibehesht", "Khordad", "Tir", "Mordad", "Shahrivar", "Mehr", "Aban", "Azar", "Dey", "Bahman", "Esfand"};
    static const char* greg[] = {"January", "February", "March", "April", "May", "June", "July", "August", "September", "October", "November", "December"};
    if (d.month < 1 || d.month > 13) return {};
    switch (c) {
    case calendar::islamic_civil: case calendar::islamic_tbla: return d.month <= 12 ? isl[d.month - 1] : "";
    case calendar::persian: return d.month <= 12 ? per[d.month - 1] : "";
    case calendar::hebrew: {
        static const char* rd[] = {"", "Nisan", "Iyar", "Sivan", "Tamuz", "Av", "Elul", "Tishri", "Heshvan", "Kislev", "Tevet", "Shevat", "Adar", "Adar II"};
        int m = heb_rd_month(d.year, d.month);
        if (m == 12 && heb_leap(d.year)) return "Adar I";
        return rd[m];
    }
    default: return d.month <= 12 ? greg[d.month - 1] : "";
    }
}
std::string era_name(calendar c, int era) {
    switch (c) {
    case calendar::japanese: return era >= 0 && era <= 4 ? jeras[era].name : "";
    case calendar::buddhist: return "BE";
    case calendar::roc: return era == 1 ? "Minguo" : "Before R.O.C.";
    case calendar::islamic_civil: case calendar::islamic_tbla: return "AH";
    case calendar::hebrew: return "AM";
    case calendar::persian: return "AP";
    default: return era == 0 ? "BC" : "AD";
    }
}
iso_week to_iso_week(int y, int m, int d) {
    std::int64_t days = days_from_civil(y, m, d);
    int wd = int(((days % 7) + 10) % 7) + 1;   // 1 = Monday (1970-01-01 Thursday = 4)
    std::int64_t thursday = days - wd + 4;
    int ty = civil_from_days(thursday)[0];
    int week = int((thursday - days_from_civil(ty, 1, 1)) / 7) + 1;
    return {ty, week, wd};
}
int week_of_year(int y, int m, int d, std::string_view locale) {
    int first = int(first_day_of_week(locale)), mind = min_days_in_first_week(locale);   // first: 0 = Sunday
    std::int64_t days = days_from_civil(y, m, d);
    auto week_start = [&](std::int64_t x) { int wd = int(((x % 7) + 11) % 7); return x - ((wd - first + 7) % 7); };
    auto year_week1 = [&](int yy) { std::int64_t j1 = days_from_civil(yy, 1, 1), s = week_start(j1); return (j1 - s) <= 7 - mind ? s : s + 7; };
    std::int64_t w1 = year_week1(y);
    if (days < w1) w1 = year_week1(y - 1);
    else if (std::int64_t n = year_week1(y + 1); days >= n) w1 = n;
    return int((days - w1) / 7) + 1;
}
}  // namespace forge::os::locale
