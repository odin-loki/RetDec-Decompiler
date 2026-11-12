// forge::os::locale: time zones. std::chrono::tzdb where the standard library ships one (MSVC), otherwise the
// system zoneinfo (TZif v1-v4 plus the POSIX TZ footer for times past the last transition).
#include "locale_internal.hpp"
#include <algorithm>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <sstream>
#include <cctype>
#include <cstdio>
#if defined(__ANDROID__)
#  include <sys/system_properties.h>
#endif
#if defined(_WIN32)
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  include <windows.h>
#else
#  include <fcntl.h>
#  include <sys/stat.h>
#  include <unistd.h>
#endif
#if defined(_MSC_VER) && defined(__cpp_lib_chrono) && __cpp_lib_chrono >= 201907L
#  define FORGE_TZ_CHRONO 1
#endif

namespace forge::os::locale {
using namespace detail;
namespace fs = std::filesystem;

namespace {
std::int64_t days_civil(int y, int m, int d) { return days_from_civil(y, m, d); }

// ---- POSIX TZ strings ("AEST-10AEDT,M10.1.0,M4.1.0/3")
struct posix_rule { char kind = 0; int month = 0, week = 0, wday = 0, day = 0; int time = 7200; };
struct posix_tz { std::string std_name, dst_name; int std_off = 0, dst_off = 0; bool has_dst = false; posix_rule start, end; bool ok = false; };
bool parse_name(std::string_view& s, std::string& out) {
    if (!s.empty() && s[0] == '<') { auto e = s.find('>'); if (e == std::string_view::npos) return false; out = s.substr(1, e - 1); s.remove_prefix(e + 1); return true; }
    std::size_t i = 0; while (i < s.size() && std::isalpha((unsigned char)s[i])) ++i;
    if (i < 3) return false; out = s.substr(0, i); s.remove_prefix(i); return true;
}
bool parse_hms(std::string_view& s, int& out, bool allow_sign) {
    int sign = 1;
    if (allow_sign && !s.empty() && (s[0] == '+' || s[0] == '-')) { if (s[0] == '-') sign = -1; s.remove_prefix(1); }
    int parts[3] = {0, 0, 0}, n = 0;
    while (n < 3) {
        std::size_t i = 0; int v = 0; while (i < s.size() && std::isdigit((unsigned char)s[i])) v = v * 10 + (s[i++] - '0');
        if (!i) return n > 0;
        parts[n++] = v; s.remove_prefix(i);
        if (!s.empty() && s[0] == ':') s.remove_prefix(1); else break;
    }
    out = sign * (parts[0] * 3600 + parts[1] * 60 + parts[2]); return true;
}
bool parse_rule(std::string_view& s, posix_rule& r) {
    if (s.empty()) return false;
    if (s[0] == 'M') {
        s.remove_prefix(1); r.kind = 'M';
        if (std::sscanf(std::string(s).c_str(), "%d.%d.%d", &r.month, &r.week, &r.wday) != 3) return false;
        while (!s.empty() && s[0] != '/' && s[0] != ',') s.remove_prefix(1);
    } else {
        r.kind = s[0] == 'J' ? 'J' : 'n'; if (s[0] == 'J') s.remove_prefix(1);
        std::size_t i = 0; while (i < s.size() && std::isdigit((unsigned char)s[i])) r.day = r.day * 10 + (s[i++] - '0'); s.remove_prefix(i);
    }
    if (!s.empty() && s[0] == '/') { s.remove_prefix(1); if (!parse_hms(s, r.time, true)) return false; }
    return true;
}
posix_tz parse_posix(std::string_view s) {
    posix_tz z;
    if (!parse_name(s, z.std_name) || !parse_hms(s, z.std_off, true)) return z;
    z.std_off = -z.std_off;
    if (s.empty()) { z.ok = true; return z; }
    if (!parse_name(s, z.dst_name)) return z;
    z.has_dst = true; z.dst_off = z.std_off + 3600;
    if (!s.empty() && s[0] != ',') { int o; if (!parse_hms(s, o, true)) return z; z.dst_off = -o; }
    if (s.empty()) { z.start = {'M', 3, 2, 0, 0, 7200}; z.end = {'M', 11, 1, 0, 0, 7200}; z.ok = true; return z; }   // US default
    if (s[0] != ',') return z; s.remove_prefix(1);
    if (!parse_rule(s, z.start)) return z;
    if (s.empty() || s[0] != ',') return z; s.remove_prefix(1);
    if (!parse_rule(s, z.end)) return z;
    z.ok = true; return z;
}
// Local seconds (in the offset in force before the transition) of a rule in year y.
std::int64_t rule_time(const posix_rule& r, int y) {
    std::int64_t day;
    if (r.kind == 'M') {
        std::int64_t first = days_civil(y, r.month, 1);
        int wd_first = int(((first % 7) + 11) % 7);   // 1970-01-01 was Thursday (4)
        int d = 1 + (r.wday - wd_first + 7) % 7 + (r.week - 1) * 7;
        static const int mdays[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
        int dim = mdays[r.month - 1] + (r.month == 2 && ((y % 4 == 0 && y % 100 != 0) || y % 400 == 0));
        while (d > dim) d -= 7;
        day = first + d - 1;
    } else if (r.kind == 'J') {
        bool leap = (y % 4 == 0 && y % 100 != 0) || y % 400 == 0;
        day = days_civil(y, 1, 1) + r.day - 1 + (leap && r.day >= 60 ? 1 : 0);
    } else day = days_civil(y, 1, 1) + r.day;
    return day * 86400 + r.time;
}
zone_info posix_at(const posix_tz& z, std::int64_t t) {
    if (!z.has_dst) return {std::chrono::seconds(z.std_off), false, z.std_name};
    auto ymd = civil_from_days((t + z.std_off) / 86400 - ((t + z.std_off) % 86400 < 0));
    int y = ymd[0];
    for (int yy : {y - 1, y, y + 1}) (void)yy;
    std::int64_t s = rule_time(z.start, y) - z.std_off, e = rule_time(z.end, y) - z.dst_off;
    bool dst = s < e ? (t >= s && t < e) : !(t >= e && t < s);   // southern hemisphere wraps the year
    return dst ? zone_info{std::chrono::seconds(z.dst_off), true, z.dst_name} : zone_info{std::chrono::seconds(z.std_off), false, z.std_name};
}

// ---- TZif
struct tzif {
    std::vector<std::int64_t> trans; std::vector<std::uint8_t> idx;
    struct ttype { int off; bool dst; std::string abbr; };
    std::vector<ttype> types; posix_tz footer; bool has_footer = false;
    zone_info at(std::int64_t t) const {
        if (trans.empty() || t < trans.front()) {
            if (trans.empty() && has_footer) return posix_at(footer, t);
            std::size_t k = 0; for (std::size_t i = 0; i < types.size(); ++i) if (!types[i].dst) { k = i; break; }
            if (types.empty()) return {};
            return {std::chrono::seconds(types[k].off), types[k].dst, types[k].abbr};
        }
        if (t >= trans.back() && has_footer) return posix_at(footer, t);
        auto it = std::upper_bound(trans.begin(), trans.end(), t); std::size_t i = std::size_t(it - trans.begin()) - 1;
        auto& ty = types[idx[i]]; return {std::chrono::seconds(ty.off), ty.dst, ty.abbr};
    }
};
std::uint32_t be32(const unsigned char* p) { return std::uint32_t(p[0]) << 24 | std::uint32_t(p[1]) << 16 | std::uint32_t(p[2]) << 8 | p[3]; }
std::int64_t be64(const unsigned char* p) { return std::int64_t(std::uint64_t(be32(p)) << 32 | be32(p + 4)); }
std::optional<tzif> parse_tzif(const std::string& d) {
    auto u = reinterpret_cast<const unsigned char*>(d.data());
    if (d.size() < 44 || d.compare(0, 4, "TZif") != 0) return std::nullopt;
    auto counts = [&](std::size_t at, std::uint32_t c[6]) { for (int i = 0; i < 6; ++i) c[i] = be32(u + at + 20 + 4 * i); };
    std::uint32_t c[6]; counts(0, c);   // isutcnt isstdcnt leapcnt timecnt typecnt charcnt
    std::size_t pos = 44; int tsize = 4; char ver = d[4];
    if (ver >= '2') {
        pos += c[3] * 4 + c[3] + c[4] * 6 + c[5] + c[2] * 8 + c[1] + c[0];
        if (d.size() < pos + 44) return std::nullopt;
        counts(pos, c); pos += 44; tsize = 8;
    }
    tzif z;
    std::size_t need = c[3] * tsize + c[3] + c[4] * 6 + c[5];
    if (d.size() < pos + need) return std::nullopt;
    for (std::uint32_t i = 0; i < c[3]; ++i) z.trans.push_back(tsize == 8 ? be64(u + pos + i * 8) : std::int64_t(std::int32_t(be32(u + pos + i * 4))));
    pos += c[3] * tsize;
    for (std::uint32_t i = 0; i < c[3]; ++i) z.idx.push_back(u[pos + i]);
    pos += c[3];
    std::size_t types_at = pos, abbr_at = pos + c[4] * 6;
    for (std::uint32_t i = 0; i < c[4]; ++i) {
        auto p = u + types_at + i * 6; std::size_t ai = p[5];
        std::string ab; for (std::size_t k = abbr_at + ai; k < abbr_at + c[5] && d[k]; ++k) ab += d[k];
        z.types.push_back({std::int32_t(be32(p)), p[4] != 0, ab});
    }
    for (auto i : z.idx) if (i >= z.types.size()) return std::nullopt;
    pos = abbr_at + c[5] + c[2] * (tsize + 4) + c[1] + c[0];
    if (ver >= '2' && pos < d.size() && d[pos] == '\n') {
        auto e = d.find('\n', pos + 1);
        if (e != std::string::npos) { z.footer = parse_posix(std::string_view(d).substr(pos + 1, e - pos - 1)); z.has_footer = z.footer.ok; }
    }
    return z;
}
std::vector<std::string> tz_dirs() {
    std::vector<std::string> r;
    if (const char* d = std::getenv("TZDIR")) r.push_back(d);
    for (auto d : {"/usr/share/zoneinfo", "/usr/lib/zoneinfo", "/usr/share/lib/zoneinfo", "/var/db/timezone/zoneinfo", "/etc/zoneinfo"}) r.push_back(d);
    return r;
}
std::string tz_dir() {
    static std::string d = [] { for (auto& x : tz_dirs()) { std::error_code ec; if (fs::exists(fs::path(x) / "UTC", ec) || fs::exists(fs::path(x) / "Etc" / "UTC", ec)) return x; } return std::string(); }();
    return d;
}

// Zone files are read only when they are regular files of at most 1 MiB (a name like "../../dev/zero" or a FIFO
// must neither exhaust memory nor block). Opened non-blocking and checked on the descriptor, so a file swapped
// for a FIFO between a stat and the open cannot hang the read.
constexpr std::uintmax_t k_max_zone_file = 1u << 20;
bool read_regular(const fs::path& p, std::string& out, std::uintmax_t max, std::uint64_t offset = 0, std::uint64_t len = UINT64_MAX) {
#if defined(_WIN32)
    std::error_code ec; auto st = fs::status(p, ec);
    if (ec || !fs::is_regular_file(st)) return false;
    auto size = fs::file_size(p, ec); if (ec || (len == UINT64_MAX && size > max)) return false;
    std::ifstream f(p, std::ios::binary); if (!f) return false;
#else
    int fd = ::open(p.c_str(), O_RDONLY | O_NONBLOCK | O_CLOEXEC | O_NOCTTY); if (fd < 0) return false;
    struct stat st {};
    if (::fstat(fd, &st) != 0 || !S_ISREG(st.st_mode) || (len == UINT64_MAX && std::uintmax_t(st.st_size) > max)) { ::close(fd); return false; }
    std::uint64_t size = std::uint64_t(st.st_size);
#endif
    if (len == UINT64_MAX) { offset = 0; len = size; }
    if (offset > size || len > size - offset || len > max) {
#if !defined(_WIN32)
        ::close(fd);
#endif
        return false;
    }
    out.assign(std::size_t(len), '\0');
#if defined(_WIN32)
    f.seekg(std::streamoff(offset)); f.read(out.data(), std::streamsize(len)); return f.gcount() == std::streamsize(len);
#else
    std::size_t got = 0;
    while (got < len) {
        auto n = ::pread(fd, out.data() + got, std::size_t(len) - got, off_t(offset + got));
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) break;
        got += std::size_t(n);
    }
    ::close(fd); return got == len;
#endif
}

// IANA-style names only: letters, digits, '_', '+', '-', '/' with no empty, "." or ".." component.
bool valid_zone_name(std::string_view n) {
    if (n.empty() || n.size() > 255 || n.front() == '/' || n.back() == '/') return false;
    for (char c : n) if (!(std::isalnum((unsigned char)c) || c == '_' || c == '+' || c == '-' || c == '/')) return false;
    return n.find("//") == std::string_view::npos;   // '.' is not in the set, so "..", "." components cannot occur
}
// An absolute zone path is trusted only when it is what $TZ names (":/etc/localtime", "/opt/tz/Custom").
bool is_abs_path(std::string_view n) { return !n.empty() && (n.front() == '/' || fs::path(std::string(n)).is_absolute()); }
std::string tz_env_path() {
    const char* tz = std::getenv("TZ"); if (!tz || !*tz) return {};
    std::string s = tz[0] == ':' ? tz + 1 : tz;
    return is_abs_path(s) && s.find("zoneinfo/") == std::string::npos ? s : std::string();
}

// Android keeps every zone in one bundle: "tzdata2024a\0", then big-endian offsets of the index, the data and the
// final section; index entries are 52 bytes (name[40], offset from data start, length, unused).
std::vector<std::string> tz_bundles() {
    std::vector<std::string> r;
    if (const char* d = std::getenv("TZDIR"); d && *d) r.push_back(std::string(d) + "/tzdata");
#if defined(__ANDROID__)
    if (const char* d = std::getenv("ANDROID_TZDATA_ROOT"); d && *d) r.push_back(std::string(d) + "/etc/tz/tzdata");
    for (auto d : {"/apex/com.android.tzdata/etc/tz/tzdata", "/data/misc/zoneinfo/current/tzdata", "/system/usr/share/zoneinfo/tzdata"}) r.push_back(d);
#endif
    return r;
}
struct bundle_entry { std::string name; std::uint32_t off, len; };
std::optional<std::vector<bundle_entry>> bundle_index(const std::string& path, std::uint32_t& data_at) {
    std::string h;
    if (!read_regular(path, h, 24, 0, 24) || h.compare(0, 6, "tzdata") != 0 || h[11] != '\0') return std::nullopt;
    auto u = reinterpret_cast<const unsigned char*>(h.data());
    std::uint32_t idx = be32(u + 12), data = be32(u + 16);
    if (idx < 24 || data < idx || (data - idx) % 52 != 0 || data - idx > 52u * 4096) return std::nullopt;
    std::string ix; if (!read_regular(path, ix, 52u * 4096, idx, data - idx)) return std::nullopt;
    std::vector<bundle_entry> r; auto p = reinterpret_cast<const unsigned char*>(ix.data());
    for (std::size_t i = 0; i + 52 <= ix.size(); i += 52) {
        std::string name(ix.data() + i, 40); name.resize(std::strlen(name.c_str()));
        r.push_back({name, be32(p + i + 40), be32(p + i + 44)});
    }
    data_at = data; return r;
}
std::optional<tzif> bundle_zone(std::string_view name) {
    for (auto& b : tz_bundles()) {
        std::uint32_t data = 0; auto ix = bundle_index(b, data); if (!ix) continue;
        for (auto& e : *ix) if (e.name == name) {
            std::string z; if (e.len > k_max_zone_file || !read_regular(b, z, k_max_zone_file, std::uint64_t(data) + e.off, e.len)) return std::nullopt;
            return parse_tzif(z);
        }
    }
    return std::nullopt;
}

#if defined(_WIN32)
// CLDR windowsZones (territory 001): Windows time zone key -> IANA zone.
constexpr std::pair<const char*, const char*> k_win_zones[] = {
    {"Dateline Standard Time", "Etc/GMT+12"}, {"UTC-11", "Etc/GMT+11"}, {"Aleutian Standard Time", "America/Adak"},
    {"Hawaiian Standard Time", "Pacific/Honolulu"}, {"Marquesas Standard Time", "Pacific/Marquesas"}, {"Alaskan Standard Time", "America/Anchorage"},
    {"UTC-09", "Etc/GMT+9"}, {"Pacific Standard Time (Mexico)", "America/Tijuana"}, {"UTC-08", "Etc/GMT+8"},
    {"Pacific Standard Time", "America/Los_Angeles"}, {"US Mountain Standard Time", "America/Phoenix"},
    {"Mountain Standard Time (Mexico)", "America/Mazatlan"}, {"Mountain Standard Time", "America/Denver"}, {"Yukon Standard Time", "America/Whitehorse"},
    {"Central America Standard Time", "America/Guatemala"}, {"Central Standard Time", "America/Chicago"}, {"Easter Island Standard Time", "Pacific/Easter"},
    {"Central Standard Time (Mexico)", "America/Mexico_City"}, {"Canada Central Standard Time", "America/Regina"}, {"SA Pacific Standard Time", "America/Bogota"},
    {"Eastern Standard Time (Mexico)", "America/Cancun"}, {"Eastern Standard Time", "America/New_York"}, {"Haiti Standard Time", "America/Port-au-Prince"},
    {"Cuba Standard Time", "America/Havana"}, {"US Eastern Standard Time", "America/Indiana/Indianapolis"}, {"Turks And Caicos Standard Time", "America/Grand_Turk"},
    {"Paraguay Standard Time", "America/Asuncion"}, {"Atlantic Standard Time", "America/Halifax"}, {"Venezuela Standard Time", "America/Caracas"},
    {"Central Brazilian Standard Time", "America/Cuiaba"}, {"SA Western Standard Time", "America/La_Paz"}, {"Pacific SA Standard Time", "America/Santiago"},
    {"Newfoundland Standard Time", "America/St_Johns"}, {"Tocantins Standard Time", "America/Araguaina"}, {"E. South America Standard Time", "America/Sao_Paulo"},
    {"SA Eastern Standard Time", "America/Cayenne"}, {"Argentina Standard Time", "America/Argentina/Buenos_Aires"}, {"Greenland Standard Time", "America/Nuuk"},
    {"Montevideo Standard Time", "America/Montevideo"}, {"Magallanes Standard Time", "America/Punta_Arenas"}, {"Saint Pierre Standard Time", "America/Miquelon"},
    {"Bahia Standard Time", "America/Bahia"}, {"UTC-02", "Etc/GMT+2"}, {"Azores Standard Time", "Atlantic/Azores"}, {"Cape Verde Standard Time", "Atlantic/Cape_Verde"},
    {"UTC", "Etc/UTC"}, {"GMT Standard Time", "Europe/London"}, {"Greenwich Standard Time", "Atlantic/Reykjavik"}, {"Sao Tome Standard Time", "Africa/Sao_Tome"},
    {"Morocco Standard Time", "Africa/Casablanca"}, {"W. Europe Standard Time", "Europe/Berlin"}, {"Central Europe Standard Time", "Europe/Budapest"},
    {"Romance Standard Time", "Europe/Paris"}, {"Central European Standard Time", "Europe/Warsaw"}, {"W. Central Africa Standard Time", "Africa/Lagos"},
    {"Jordan Standard Time", "Asia/Amman"}, {"GTB Standard Time", "Europe/Bucharest"}, {"Middle East Standard Time", "Asia/Beirut"},
    {"Egypt Standard Time", "Africa/Cairo"}, {"E. Europe Standard Time", "Europe/Chisinau"}, {"Syria Standard Time", "Asia/Damascus"},
    {"West Bank Standard Time", "Asia/Hebron"}, {"South Africa Standard Time", "Africa/Johannesburg"}, {"FLE Standard Time", "Europe/Kyiv"},
    {"Israel Standard Time", "Asia/Jerusalem"}, {"South Sudan Standard Time", "Africa/Juba"}, {"Kaliningrad Standard Time", "Europe/Kaliningrad"},
    {"Sudan Standard Time", "Africa/Khartoum"}, {"Libya Standard Time", "Africa/Tripoli"}, {"Namibia Standard Time", "Africa/Windhoek"},
    {"Arabic Standard Time", "Asia/Baghdad"}, {"Turkey Standard Time", "Europe/Istanbul"}, {"Arab Standard Time", "Asia/Riyadh"},
    {"Belarus Standard Time", "Europe/Minsk"}, {"Russian Standard Time", "Europe/Moscow"}, {"E. Africa Standard Time", "Africa/Nairobi"},
    {"Volgograd Standard Time", "Europe/Volgograd"}, {"Iran Standard Time", "Asia/Tehran"}, {"Arabian Standard Time", "Asia/Dubai"},
    {"Astrakhan Standard Time", "Europe/Astrakhan"}, {"Azerbaijan Standard Time", "Asia/Baku"}, {"Russia Time Zone 3", "Europe/Samara"},
    {"Mauritius Standard Time", "Indian/Mauritius"}, {"Saratov Standard Time", "Europe/Saratov"}, {"Georgian Standard Time", "Asia/Tbilisi"},
    {"Caucasus Standard Time", "Asia/Yerevan"}, {"Afghanistan Standard Time", "Asia/Kabul"}, {"West Asia Standard Time", "Asia/Tashkent"},
    {"Ekaterinburg Standard Time", "Asia/Yekaterinburg"}, {"Pakistan Standard Time", "Asia/Karachi"}, {"Qyzylorda Standard Time", "Asia/Qyzylorda"},
    {"India Standard Time", "Asia/Kolkata"}, {"Sri Lanka Standard Time", "Asia/Colombo"}, {"Nepal Standard Time", "Asia/Kathmandu"},
    {"Central Asia Standard Time", "Asia/Bishkek"}, {"Bangladesh Standard Time", "Asia/Dhaka"}, {"Omsk Standard Time", "Asia/Omsk"},
    {"Myanmar Standard Time", "Asia/Yangon"}, {"SE Asia Standard Time", "Asia/Bangkok"}, {"Altai Standard Time", "Asia/Barnaul"},
    {"W. Mongolia Standard Time", "Asia/Hovd"}, {"North Asia Standard Time", "Asia/Krasnoyarsk"}, {"N. Central Asia Standard Time", "Asia/Novosibirsk"},
    {"Tomsk Standard Time", "Asia/Tomsk"}, {"China Standard Time", "Asia/Shanghai"}, {"North Asia East Standard Time", "Asia/Irkutsk"},
    {"Singapore Standard Time", "Asia/Singapore"}, {"W. Australia Standard Time", "Australia/Perth"}, {"Taipei Standard Time", "Asia/Taipei"},
    {"Ulaanbaatar Standard Time", "Asia/Ulaanbaatar"}, {"Aus Central W. Standard Time", "Australia/Eucla"}, {"Transbaikal Standard Time", "Asia/Chita"},
    {"Tokyo Standard Time", "Asia/Tokyo"}, {"North Korea Standard Time", "Asia/Pyongyang"}, {"Korea Standard Time", "Asia/Seoul"},
    {"Yakutsk Standard Time", "Asia/Yakutsk"}, {"Cen. Australia Standard Time", "Australia/Adelaide"}, {"AUS Central Standard Time", "Australia/Darwin"},
    {"E. Australia Standard Time", "Australia/Brisbane"}, {"AUS Eastern Standard Time", "Australia/Sydney"}, {"West Pacific Standard Time", "Pacific/Port_Moresby"},
    {"Tasmania Standard Time", "Australia/Hobart"}, {"Vladivostok Standard Time", "Asia/Vladivostok"}, {"Lord Howe Standard Time", "Australia/Lord_Howe"},
    {"Bougainville Standard Time", "Pacific/Bougainville"}, {"Russia Time Zone 10", "Asia/Srednekolymsk"}, {"Magadan Standard Time", "Asia/Magadan"},
    {"Norfolk Standard Time", "Pacific/Norfolk"}, {"Sakhalin Standard Time", "Asia/Sakhalin"}, {"Central Pacific Standard Time", "Pacific/Guadalcanal"},
    {"Russia Time Zone 11", "Asia/Kamchatka"}, {"New Zealand Standard Time", "Pacific/Auckland"}, {"UTC+12", "Etc/GMT-12"},
    {"Fiji Standard Time", "Pacific/Fiji"}, {"Chatham Islands Standard Time", "Pacific/Chatham"}, {"UTC+13", "Etc/GMT-13"},
    {"Tonga Standard Time", "Pacific/Tongatapu"}, {"Samoa Standard Time", "Pacific/Apia"}, {"Line Islands Standard Time", "Pacific/Kiritimati"}};
std::wstring widen(std::string_view s) { std::wstring w; for (char c : s) w += wchar_t((unsigned char)c); return w; }   // key names are ASCII
std::string narrow(const wchar_t* w) { std::string s; for (; *w; ++w) s += *w < 128 ? char(*w) : '?'; return s; }
// Current rules of a Windows zone from HKLM\...\Time Zones\<key>\TZI (REG_TZI_FORMAT), as a POSIX rule pair.
std::optional<tzif> registry_zone(std::string_view name) {
    std::string key(name);
    for (auto& [w, i] : k_win_zones) if (name == i) { key = w; break; }
    if (key.empty() || key.find_first_of("\\/") != std::string::npos || key.size() > 128) return std::nullopt;
    struct reg_tzi { LONG bias, std_bias, dlt_bias; SYSTEMTIME std_date, dlt_date; } t{};
    DWORD size = sizeof t;
    auto path = L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\Time Zones\\" + widen(key);
    if (RegGetValueW(HKEY_LOCAL_MACHINE, path.c_str(), L"TZI", RRF_RT_REG_BINARY, nullptr, &t, &size) != ERROR_SUCCESS || size != sizeof t) return std::nullopt;
    auto abbr = [](int off) { int a = off < 0 ? -off : off; char b[16]; std::snprintf(b, sizeof b, a % 3600 ? "%c%02d%02d" : "%c%02d", off < 0 ? '-' : '+', a / 3600, a % 3600 / 60); return std::string(b); };
    tzif z; z.has_footer = true; auto& p = z.footer; p.ok = true;
    p.std_off = -int(t.bias + t.std_bias) * 60; p.std_name = abbr(p.std_off);
    if (t.dlt_date.wMonth && t.std_date.wMonth && t.dlt_bias != t.std_bias) {
        p.has_dst = true; p.dst_off = -int(t.bias + t.dlt_bias) * 60; p.dst_name = abbr(p.dst_off);
        auto rule = [](const SYSTEMTIME& s) { return posix_rule{'M', s.wMonth, s.wDay, s.wDayOfWeek, 0, s.wHour * 3600 + s.wMinute * 60 + s.wSecond}; };
        p.start = rule(t.dlt_date); p.end = rule(t.std_date);
    }
    return z;
}
std::string windows_local_zone() {
    DYNAMIC_TIME_ZONE_INFORMATION d{};
    if (GetDynamicTimeZoneInformation(&d) == TIME_ZONE_ID_INVALID || !d.TimeZoneKeyName[0]) return {};
    auto key = narrow(d.TimeZoneKeyName);
    for (auto& [w, i] : k_win_zones) if (key == w) return i;
    return key;   // unmapped: the registry key itself, which zone_at resolves through the registry
}
#endif

// "Etc/GMT+5" (POSIX sign: five hours west) without any zone files.
std::optional<tzif> etc_gmt(std::string_view n) {
    if (!n.starts_with("Etc/GMT") || n.size() < 9 || (n[7] != '+' && n[7] != '-')) return std::nullopt;
    int h = 0; for (char c : n.substr(8)) { if (!std::isdigit((unsigned char)c)) return std::nullopt; h = h * 10 + (c - '0'); }
    if (h > 14) return std::nullopt;
    tzif z; z.has_footer = true; z.footer.ok = true; z.footer.std_off = (n[7] == '+' ? -h : h) * 3600;
    char b[16]; std::snprintf(b, sizeof b, "%c%02d", n[7] == '+' ? '-' : '+', h); z.footer.std_name = b;
    return z;
}

// Parsed zones, bounded; only successes are cached (a failure may be a zone file that is being replaced).
constexpr std::size_t k_zone_cache_max = 64;
std::mutex z_mu; std::map<std::string, std::shared_ptr<const tzif>, std::less<>> z_cache;
std::shared_ptr<const tzif> load_zone(std::string_view name) {
    { std::lock_guard lk(z_mu); if (auto it = z_cache.find(name); it != z_cache.end()) return it->second; }
    std::optional<tzif> z;
    std::string data;
    if (is_abs_path(name)) {
        if (name == tz_env_path() && read_regular(fs::path(std::string(name)), data, k_max_zone_file)) z = parse_tzif(data);
    } else if (valid_zone_name(name)) {
        if (!tz_dir().empty() && read_regular(fs::path(tz_dir()) / std::string(name), data, k_max_zone_file)) z = parse_tzif(data);
        if (!z) z = bundle_zone(name);
#if defined(_WIN32)
        if (!z) z = registry_zone(name);
#endif
        if (!z) z = etc_gmt(name);
    }
    if (!z && !is_abs_path(name)) {   // a bare POSIX TZ string ("EST5EDT,M3.2.0,M11.1.0")
        auto p = parse_posix(name);
        if (p.ok) { z.emplace(); z->footer = p; z->has_footer = true; }
    }
    if (!z) return nullptr;
    auto res = std::make_shared<const tzif>(std::move(*z));
    std::lock_guard lk(z_mu);
    if (z_cache.size() >= k_zone_cache_max) z_cache.clear();
    z_cache.emplace(std::string(name), res);
    return res;
}
std::string read_first_line(const char* path) { std::string d; if (!read_regular(path, d, 4096)) return {}; auto l = d.substr(0, d.find('\n')); while (!l.empty() && (l.back() == ' ' || l.back() == '\r')) l.pop_back(); return l; }
}  // namespace

std::string tz_backend() {
#if FORGE_TZ_CHRONO
    return "chrono-tzdb";
#else
    if (auto d = tz_dir(); !d.empty()) return "tzif:" + d;
    for (auto& b : tz_bundles()) { std::uint32_t data; if (bundle_index(b, data)) return "tzdata:" + b; }
#  if defined(_WIN32)
    return "windows-registry";
#  else
    return "utc-only";
#  endif
#endif
}
std::string local_zone() {
    if (const char* tz = std::getenv("TZ"); tz && *tz) {
        std::string s = tz[0] == ':' ? tz + 1 : tz;
        if (auto p = s.find("zoneinfo/"); p != std::string::npos) s = s.substr(p + 9);
        return s;
    }
#if FORGE_TZ_CHRONO
    try { return std::string(std::chrono::current_zone()->name()); } catch (...) {}
#endif
#if defined(_WIN32)
    if (auto z = windows_local_zone(); !z.empty()) return z;
#elif defined(__APPLE__)
    if (auto z = apple_local_zone(); !z.empty()) return z;
#elif defined(__ANDROID__)
    char v[PROP_VALUE_MAX] = {}; if (__system_property_get("persist.sys.timezone", v) > 0) return v;
#endif
#if !defined(_WIN32)
    std::error_code ec;
    auto target = fs::read_symlink("/etc/localtime", ec);
    if (!ec) { auto s = target.string(); if (auto p = s.find("zoneinfo/"); p != std::string::npos) return s.substr(p + 9); }
    for (auto f : {"/etc/timezone", "/var/db/zoneinfo"}) { auto l = read_first_line(f); if (!l.empty()) return l; }
    if (auto l = read_first_line("/etc/sysconfig/clock"); l.find("ZONE=") != std::string::npos) { auto v = l.substr(l.find("ZONE=") + 5); std::erase(v, '"'); return v; }
#endif
    return "UTC";
}
std::vector<std::string> zone_names() {
    std::vector<std::string> r;
#if FORGE_TZ_CHRONO
    try { for (auto& z : std::chrono::get_tzdb().zones) r.emplace_back(z.name()); } catch (...) {}
#else
    auto d = tz_dir();
    if (d.empty()) {   // Android's single-file bundle, or Windows' registry zones mapped to IANA names
        for (auto& b : tz_bundles()) { std::uint32_t data; if (auto ix = bundle_index(b, data)) { for (auto& e : *ix) r.push_back(e.name); break; } }
#  if defined(_WIN32)
        if (r.empty()) for (auto& [w, i] : k_win_zones) if (registry_zone(w)) r.emplace_back(i);
#  endif
        if (r.empty()) r.emplace_back("UTC");
        std::sort(r.begin(), r.end()); r.erase(std::unique(r.begin(), r.end()), r.end());
        return r;
    }
    std::error_code ec;
    for (auto it = fs::recursive_directory_iterator(d, ec); !ec && it != fs::recursive_directory_iterator(); it.increment(ec)) {
        auto rel = fs::relative(it->path(), d, ec).generic_string();
        if (it->is_directory(ec)) { if (rel == "posix" || rel == "right" || rel == "SystemV") it.disable_recursion_pending(); continue; }
        if (rel.empty() || !std::isupper((unsigned char)rel[0]) || rel.find('.') != std::string::npos) continue;
        std::ifstream f(it->path(), std::ios::binary); char m[4] = {}; f.read(m, 4);
        if (std::memcmp(m, "TZif", 4) == 0) r.push_back(rel);
    }
    std::sort(r.begin(), r.end());
#endif
    return r;
}
result<zone_info> zone_at(std::string_view zone, sys_seconds t) {
    result<zone_info> r; std::string z = zone.empty() ? local_zone() : std::string(zone);
    if (z == "UTC" || z == "Etc/UTC" || z == "Z" || z == "GMT" || z == "Etc/GMT") { r.value = {std::chrono::seconds(0), false, z == "GMT" ? "GMT" : "UTC"}; return r; }
#if FORGE_TZ_CHRONO
    try {
        auto info = std::chrono::locate_zone(z)->get_info(t);
        r.value = {std::chrono::duration_cast<std::chrono::seconds>(info.offset), info.save != std::chrono::minutes(0), info.abbrev};
        return r;
    } catch (...) {}
#endif
    auto tz = load_zone(z);
    if (!tz) { r.err = {ENOENT, "time zone '" + z + "' not found (" + tz_backend() + ")"}; return r; }
    r.value = tz->at(t.time_since_epoch().count());
    return r;
}
result<sys_seconds> from_local(std::string_view zone, std::int64_t local, bool earlier) {
    result<sys_seconds> r;
    auto off = [&](std::int64_t t) -> std::optional<std::int64_t> { auto z = zone_at(zone, sys_seconds(std::chrono::seconds(t))); if (!z) return std::nullopt; return z.value.offset.count(); };
    auto a = off(local - 86400), b = off(local + 86400);
    if (!a || !b) { auto z = zone_at(zone, sys_seconds{}); r.err = z.err; return r; }
    std::vector<std::int64_t> valid;
    for (auto o : {*a, *b}) { std::int64_t t = local - o; if (off(t) == o && std::find(valid.begin(), valid.end(), t) == valid.end()) valid.push_back(t); }
    std::sort(valid.begin(), valid.end());
    if (valid.empty()) r.value = sys_seconds(std::chrono::seconds(local - *a));   // in a gap: move forward by the gap
    else r.value = sys_seconds(std::chrono::seconds(earlier ? valid.front() : valid.back()));
    return r;
}
}  // namespace forge::os::locale
