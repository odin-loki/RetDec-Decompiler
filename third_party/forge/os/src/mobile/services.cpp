// forge_mobile device services: typed API over (1) the app/sim service hook, (2) native backends,
// (3) portable defaults built on forge::os (power, net, clipboard, sysinfo, process).
#include "internal.hpp"
#include <forge/json/json.hpp>
#include <forge/os/clipboard.hpp>
#include <forge/os/net.hpp>
#include <forge/os/process.hpp>
#include <forge/os/sysinfo.hpp>
#include <forge/os/memory_pressure.hpp>
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <thread>
#include <stdexcept>
#if defined(_WIN32)
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  include <windows.h>
#elif (defined(__APPLE__) && !defined(FORGE_OS_IOS)) || defined(__FreeBSD__)   // no <net/if_media.h> in the iOS SDK
#  include <net/if.h>
#  include <net/if_media.h>
#  include <sys/ioctl.h>
#  include <sys/socket.h>
#  include <unistd.h>
#endif

namespace forge::os::mobile {
using namespace detail;

static constexpr const char* k_form[] = {"unknown", "phone", "tablet", "watch", "tv", "desktop", "laptop", "car"};
const char* form_factor_name(form_factor f) { return k_form[int(f)]; }
static form_factor parse_form(std::string_view s) { for (int i = 0; i < 8; ++i) if (s == k_form[i]) return form_factor(i); return form_factor::unknown; }
static constexpr const char* k_life[] = {"foreground", "background", "low_memory", "terminate"};
const char* app_event_name(app_event e) { return k_life[int(e)]; }
std::optional<app_event> parse_app_event(std::string_view s) { for (int i = 0; i < 4; ++i) if (s == k_life[i]) return app_event(i); return std::nullopt; }

// ------------------------------------------------------------------ geofencing
double geofence_monitor::distance_m(double la1, double lo1, double la2, double lo2) {
    constexpr double r = 6371008.8, d = 3.14159265358979323846 / 180;
    double a = std::sin((la2 - la1) * d / 2), b = std::sin((lo2 - lo1) * d / 2);
    double h = a * a + std::cos(la1 * d) * std::cos(la2 * d) * b * b;
    return 2 * r * std::asin(std::min(1.0, std::sqrt(h)));
}
void geofence_monitor::add(geofence g) { remove(g.id); f_.push_back({std::move(g), -1}); }
bool geofence_monitor::remove(std::string_view id) { return std::erase_if(f_, [&](auto& x) { return x.g.id == id; }) > 0; }
std::vector<geofence_event> geofence_monitor::feed(const sensor_event& e) {
    std::vector<geofence_event> out; if (e.kind != sensor_kind::location || e.n < 2) return out;
    for (auto& f : f_) {
        int in = distance_m(e.v[0], e.v[1], f.g.lat, f.g.lon) <= f.g.radius_m;
        if (in != f.inside && (f.inside != -1 || in)) out.push_back({f.g.id, bool(in), e.t_ns});
        f.inside = in;
    }
    return out;
}

// ------------------------------------------------------------------ hooks and lifecycle
void set_service_hook(service_hook h) { auto& br = bridge::get(); std::lock_guard g(br.m); br.svc = std::move(h); }
void feed_lifecycle(app_event e) { dispatch_lifecycle(e); }

namespace detail {
void dispatch_lifecycle(app_event e, bool drive_mem) {
    if (e == app_event::low_memory && drive_mem) forge::mem::governor::instance().on_pressure(os::pressure_level::critical);
    std::vector<std::shared_ptr<std::function<void(app_event)>>> cbs;
    { auto& br = bridge::get(); std::lock_guard g(br.m);
      std::erase_if(br.life, [&](auto& w) { auto p = w.lock(); if (!p) return true; cbs.push_back(p); return false; }); }
    for (auto& c : cbs) (*c)(e);
}
std::optional<std::string> backend::svc(std::string_view op, std::string_view args) {
    service_hook h; { auto& br = bridge::get(); std::lock_guard g(br.m); h = br.svc; }
    return h ? h(op, args) : std::nullopt;
}
int run_tool(const std::vector<std::string>& argv, std::string* out, int timeout_ms) {
    spawn_spec sp; sp.argv = argv; sp.capture_stdout = out != nullptr; sp.capture_stderr = true; sp.timeout_ms = std::uint64_t(timeout_ms);
    auto r = spawn_wait(sp); if (!r) return -1;
    if (out) *out = r.value.out;
    return r.value.exit_code == 127 ? -1 : r.value.exit_code;
}

void backend::lifecycle_started() {
    static std::once_flag once;   // forge::mem already reacts to the same signal itself; just tell subscribers
    std::call_once(once, [] { memory_pressure::instance().subscribe([](pressure_level l) { if (l == pressure_level::critical) dispatch_lifecycle(app_event::low_memory, false); }); });
}
result<power_info> backend::power() { return {power_status(), {}}; }

namespace {
bool starts(const std::string& n, const char* s) { return n.rfind(s, 0) == 0; }
std::string slurp_small(const std::string& p) {
    std::ifstream f(p); std::string s; std::getline(f, s, '\0');
    while (!s.empty() && (s.back() == '\n' || s.back() == ' ')) s.pop_back(); return s;
}
// Transport of one interface: the kernel's own classification where it has one, names only as a last resort.
std::string transport_of(const std::string& n) {
#if defined(__linux__)
    std::string d = "/sys/class/net/" + n;
    std::error_code ec;
    if (std::filesystem::exists(d + "/wireless", ec) || std::filesystem::exists(d + "/phy80211", ec)) return "wifi";
    auto ue = slurp_small(d + "/uevent");
    if (ue.find("DEVTYPE=wlan") != std::string::npos) return "wifi";
    if (ue.find("DEVTYPE=wwan") != std::string::npos) return "cellular";
    if (ue.find("DEVTYPE=bluetooth") != std::string::npos) return "bluetooth";
    if (ue.find("DEVTYPE=wireguard") != std::string::npos) return "vpn";
    auto type = slurp_small(d + "/type");
    if (type == "65534" || type == "512" || type == "776") return "vpn";   // ARPHRD_NONE (tun, wireguard), PPP, SIT
    if (type == "1" && std::filesystem::exists(d + "/device", ec)) {        // a physical ARPHRD_ETHER device
        if (starts(n, "rmnet") || starts(n, "ccmni") || starts(n, "wwan")) return "cellular";
        return "ethernet";
    }
#elif (defined(__APPLE__) && !defined(FORGE_OS_IOS)) || defined(__FreeBSD__)   // no <net/if_media.h> in the iOS SDK
    // The interface's media type: IFM_IEEE80211 for Wi-Fi (macOS en0 on laptops), IFM_ETHER for wired.
    int fd = ::socket(AF_INET, SOCK_DGRAM, 0);
    if (fd >= 0) {
        ifmediareq mr{}; std::snprintf(mr.ifm_name, sizeof mr.ifm_name, "%s", n.c_str());
        int rc = ::ioctl(fd, SIOCGIFMEDIA, &mr); ::close(fd);
        if (rc == 0) { if (IFM_TYPE(mr.ifm_current) == IFM_IEEE80211) return "wifi"; if (IFM_TYPE(mr.ifm_current) == IFM_ETHER && !starts(n, "bridge") && !starts(n, "ap")) return "ethernet"; }
    }
#endif
    if (starts(n, "wl") || starts(n, "wifi") || n.find("Wi-Fi") != std::string::npos || n.find("Wireless") != std::string::npos || n.find("WLAN") != std::string::npos) return "wifi";
    if (starts(n, "wwan") || starts(n, "rmnet") || starts(n, "ccmni") || starts(n, "pdp_ip") || starts(n, "rev_rmnet") || n.find("Cellular") != std::string::npos || n.find("Mobile") != std::string::npos) return "cellular";
    if (starts(n, "tun") || starts(n, "utun") || starts(n, "wg") || starts(n, "ppp") || starts(n, "ipsec") || starts(n, "tap")) return "vpn";
    if (starts(n, "bnep") || starts(n, "bt-pan") || n.find("Bluetooth") != std::string::npos) return "bluetooth";
#if !defined(__APPLE__)   // en* is Wi-Fi on most Macs: never guessed there
    if (starts(n, "en") || starts(n, "eth") || starts(n, "em") || starts(n, "igb") || starts(n, "re") || n.find("Ethernet") != std::string::npos) return "ethernet";
#endif
    return "unknown";
}
// Link-local (169.254/16, fe80::/10) and loopback addresses do not make a device online.
bool routable(const net::address& a) {
    auto ip = a.ip; for (auto& c : ip) c = char(std::tolower((unsigned char)c));
    if (a.family == 4) return !starts(ip, "169.254.") && !starts(ip, "127.") && ip != "0.0.0.0";
    return !(starts(ip, "fe8") || starts(ip, "fe9") || starts(ip, "fea") || starts(ip, "feb")) && ip != "::1" && ip != "::";
}
}  // namespace

result<connectivity_info> backend::connectivity() {
    auto l = net::interfaces(); if (!l) return {{}, l.err};
    connectivity_info c; int best = 99; static const char* rank[] = {"ethernet", "wifi", "cellular", "vpn", "bluetooth", "unknown"};
    for (auto& i : l.value) {
        if (i.loopback || !i.up || std::none_of(i.addresses.begin(), i.addresses.end(), routable)) continue;
        c.online = true; c.interfaces.push_back(i.name); auto t = transport_of(i.name);
        for (int r = 0; r < 6; ++r) if (t == rank[r] && r < best) { best = r; c.transport = t; }
    }
    if (!c.online) c.transport = "none";
    c.metered = c.transport == "cellular";
    return {c, {}};
}
result<std::string> backend::clipboard_get() { return clipboard::get(); }
status backend::clipboard_set(std::string_view t) { return clipboard::set(std::string(t)); }
result<device_info> backend::info() {
    auto r = release_info(); device_info d; d.os_name = r.name.empty() ? r.id : r.name; d.os_version = r.version.empty() ? r.kernel_version : r.version; d.arch = r.arch;
    switch (current()) { case platform::android: case platform::ios: d.form = form_factor::phone; break; case platform::windows: case platform::macos: case platform::linux: case platform::freebsd: d.form = form_factor::desktop; break; default: break; }
    return {d, {}};
}

#if defined(_WIN32)
namespace {
// Known folders through shell32 (and CoTaskMemFree from ole32) loaded at run time: redirected Documents/Downloads
// (OneDrive, group policy) are honoured, which %USERPROFILE%\Documents is not.
std::string known_folder(const GUID& id) {
    using get_t = HRESULT(WINAPI*)(const GUID&, DWORD, HANDLE, PWSTR*); using free_t = void(WINAPI*)(void*);
    static auto get = reinterpret_cast<get_t>(reinterpret_cast<void*>(GetProcAddress(LoadLibraryExW(L"shell32.dll", nullptr, 0x800 /*LOAD_LIBRARY_SEARCH_SYSTEM32*/), "SHGetKnownFolderPath")));
    static auto release = reinterpret_cast<free_t>(reinterpret_cast<void*>(GetProcAddress(LoadLibraryExW(L"ole32.dll", nullptr, 0x800 /*LOAD_LIBRARY_SEARCH_SYSTEM32*/), "CoTaskMemFree")));
    if (!get) return {};
    PWSTR p = nullptr; std::string r;
    if (SUCCEEDED(get(id, 0 /*KF_FLAG_DEFAULT*/, nullptr, &p)) && p) {
        int n = WideCharToMultiByte(CP_UTF8, 0, p, -1, nullptr, 0, nullptr, nullptr);
        if (n > 1) { r.resize(std::size_t(n - 1)); WideCharToMultiByte(CP_UTF8, 0, p, -1, r.data(), n, nullptr, nullptr); }
    }
    if (p && release) release(p);
    return r;
}
constexpr GUID k_profile{0x5E6C858F, 0x0E22, 0x4760, {0x9A, 0xFE, 0xEA, 0x33, 0x17, 0xB6, 0x71, 0x73}};
constexpr GUID k_documents{0xFDD39AD0, 0x238F, 0x46AF, {0xAD, 0xB4, 0x6C, 0x85, 0x48, 0x03, 0x69, 0xC7}};
constexpr GUID k_downloads{0x374DE290, 0x123F, 0x4565, {0x91, 0x64, 0x39, 0xC4, 0x92, 0x5E, 0x46, 0x7B}};
constexpr GUID k_roaming{0x3EB685DB, 0x65F9, 0x4CF6, {0xA0, 0x3A, 0xE3, 0xEF, 0x65, 0x72, 0x9F, 0x3D}};
constexpr GUID k_local{0xF1B32785, 0x6FBA, 0x4FCF, {0x9D, 0x55, 0x7B, 0x8E, 0x7F, 0x15, 0x70, 0x91}};
}  // namespace
#elif !defined(__APPLE__) && !defined(__ANDROID__)
namespace {
// xdg-user-dirs: XDG_DOCUMENTS_DIR="$HOME/Documents" lines in $XDG_CONFIG_HOME/user-dirs.dirs (not environment variables).
std::string user_dir(const std::string& home, const char* key) {
    auto cfg = os::getenv("XDG_CONFIG_HOME"); if (cfg.empty()) cfg = home + "/.config";
    std::ifstream f(cfg + "/user-dirs.dirs"); std::string line, want = std::string(key) + "=";
    while (std::getline(f, line)) {
        auto a = line.find_first_not_of(" \t"); if (a == std::string::npos || line[a] == '#' || line.compare(a, want.size(), want)) continue;
        auto v = line.substr(a + want.size()); while (!v.empty() && (v.back() == '\r' || v.back() == ' ')) v.pop_back();
        if (v.size() >= 2 && v.front() == '"' && v.back() == '"') v = v.substr(1, v.size() - 2);
        if (v.rfind("$HOME", 0) == 0) v = home + v.substr(5);
        if (v.empty() || v.front() != '/' || v == home || v == home + "/") return {};   // "$HOME/" means the directory is disabled
        return v;
    }
    return {};
}
}  // namespace
#endif
result<storage_paths> backend::paths() {
    storage_paths p; auto env = [](const char* k) { return os::getenv(k); };
#if defined(_WIN32)
    p.home = known_folder(k_profile); if (p.home.empty()) p.home = env("USERPROFILE");
    p.documents = known_folder(k_documents); if (p.documents.empty()) p.documents = p.home + "\\Documents";
    p.downloads = known_folder(k_downloads); if (p.downloads.empty()) p.downloads = p.home + "\\Downloads";
    p.data = known_folder(k_roaming); if (p.data.empty()) p.data = env("APPDATA");
    p.cache = known_folder(k_local); if (p.cache.empty()) p.cache = env("LOCALAPPDATA");
    wchar_t t[MAX_PATH + 1]; DWORD n = GetTempPathW(MAX_PATH + 1, t);
    if (n > 0 && n <= MAX_PATH) { int m = WideCharToMultiByte(CP_UTF8, 0, t, int(n), nullptr, 0, nullptr, nullptr); p.temp.resize(std::size_t(m)); WideCharToMultiByte(CP_UTF8, 0, t, int(n), p.temp.data(), m, nullptr, nullptr); while (p.temp.size() > 3 && (p.temp.back() == '\\' || p.temp.back() == '/')) p.temp.pop_back(); }
    else p.temp = env("TEMP");
#elif defined(__ANDROID__)
    // An app's storage (filesDir, cacheDir, getExternalFilesDir) is only known to Java/Kotlin: the glue answers the
    // "paths" service hook. Without it only a process started with HOME/TMPDIR (adb shell, Termux) has paths.
    p.home = env("HOME"); p.temp = env("TMPDIR");
    if (p.home.empty() || p.temp.empty()) return {{}, unsupported("storage paths on Android (answer the \"paths\" service hook from the app glue)")};
    p.documents = p.home + "/Documents"; p.downloads = p.home + "/Download"; p.data = p.home; p.cache = p.temp;
#else
    p.home = env("HOME"); p.temp = env("TMPDIR").empty() ? "/tmp" : env("TMPDIR");
    if (p.home.empty()) return {{}, {2, "paths: HOME is not set"}};
#if defined(__APPLE__)
    p.documents = p.home + "/Documents"; p.downloads = p.home + "/Downloads"; p.data = p.home + "/Library/Application Support"; p.cache = p.home + "/Library/Caches";
#else
    auto x = [&](const char* k, std::string d) { auto v = env(k); return v.empty() ? d : v; };
    p.documents = user_dir(p.home, "XDG_DOCUMENTS_DIR"); if (p.documents.empty()) p.documents = p.home + "/Documents";
    p.downloads = user_dir(p.home, "XDG_DOWNLOAD_DIR"); if (p.downloads.empty()) p.downloads = p.home + "/Downloads";
    p.data = x("XDG_DATA_HOME", p.home + "/.local/share"); p.cache = x("XDG_CACHE_HOME", p.home + "/.cache");
#endif
#endif
    return {p, {}};
}

// ---- open_url: an absolute URL whose RFC 3986 scheme is on the allow-list
namespace {
std::mutex g_scheme_mu; std::vector<std::string> g_schemes{"http", "https", "mailto", "tel"};
}
status check_url(std::string_view u) {
    if (u.empty() || u.size() > 8192) return {{}, {22, "open_url: empty or too long"}};
    for (unsigned char c : u) if (c <= 0x20 || c == 0x7F) return {{}, {22, "open_url: control character or space in URL"}};
    // scheme = ALPHA *( ALPHA / DIGIT / "+" / "-" / "." ) ":" at position 0 (so never a path or a leading '-' option)
    if (!std::isalpha((unsigned char)u[0])) return {{}, {22, "open_url: not an absolute URL"}};
    std::size_t i = 1; while (i < u.size() && (std::isalnum((unsigned char)u[i]) || u[i] == '+' || u[i] == '-' || u[i] == '.')) ++i;
    if (i >= u.size() || u[i] != ':' || i + 1 == u.size()) return {{}, {22, "open_url: not an absolute URL"}};
    std::string scheme(u.substr(0, i)); for (auto& c : scheme) c = char(std::tolower((unsigned char)c));
    { std::lock_guard g(g_scheme_mu); if (std::find(g_schemes.begin(), g_schemes.end(), scheme) == g_schemes.end()) return {{}, {1, "open_url: scheme '" + scheme + "' is not allowed (set_url_schemes)"}}; }
    if ((scheme == "http" || scheme == "https") && (u.substr(i + 1, 2) != "//" || u.size() <= i + 3 || u[i + 3] == '/')) return {{}, {22, "open_url: " + scheme + " URL without a host"}};
    return {};
}
status backend::open_url(std::string_view u) {
    if (auto c = check_url(u); !c) return c;
    std::string url(u);
#if defined(__APPLE__)
    int rc = run_tool({"open", url});
#elif defined(_WIN32)
    int rc = run_tool({"rundll32", "url.dll,FileProtocolHandler", url});
#else
    int rc = run_tool({"xdg-open", url});
#endif
    if (rc < 0) return {{}, unsupported("open_url (no opener)")};
    return rc == 0 ? status{} : status{{}, {rc, "open_url: opener failed"}};
}
} // namespace detail
void set_url_schemes(std::vector<std::string> schemes) {
    for (auto& s : schemes) for (auto& c : s) c = char(std::tolower((unsigned char)c));
    std::lock_guard g(detail::g_scheme_mu); detail::g_schemes = std::move(schemes);
}

// ------------------------------------------------------------------ device methods: hook/sim first, then native
namespace {
std::string jstr(std::string_view s) { std::string o; json::dump(o, json::value(std::string(s))); return o; }
template <class T, class F> result<T> decode(const std::optional<std::string>& s, F f) {
    try { auto v = json::parse(*s);
          if (v.is_obj()) if (auto* u = v.find("unsupported")) return {{}, unsupported(u->str())};
          return {f(v), {}}; } catch (const std::exception& e) { return {{}, {22, std::string("service hook: ") + e.what()}}; }
}
status decode_status(const std::optional<std::string>& s) {
    try { auto v = json::parse(*s);
        if (v.is_bool() && !v.b()) return {{}, {-1, "service hook: refused"}};
        if (v.is_obj()) { if (auto* e = v.find("error")) return {{}, {int(v.num_or("code", -1)), e->str()}}; if (auto* u = v.find("unsupported")) return {{}, unsupported(u->str())}; }
        return {};
    } catch (const std::exception& e) { return {{}, {22, std::string("service hook: ") + e.what()}}; }
}
}
#define FORGE_SVC_NEED() if (!b_) return {{}, {22, "mobile: empty device"}}

result<power_info> device::power() {
    FORGE_SVC_NEED();
    if (auto s = b_->svc("power", "{}")) return decode<power_info>(s, [](const json::value& v) {
        power_info p; p.has_battery = v.find("has_battery") ? v["has_battery"].b() : true; p.battery_percent = int(v.num_or("battery_percent", -1));
        p.charging = v.find("charging") && v["charging"].b(); p.on_battery = v.find("on_battery") && v["on_battery"].b();
        p.low_power_mode = v.find("low_power_mode") && v["low_power_mode"].b(); p.temperature_c = v.num_or("temperature_c", -1);
        auto t = v.str_or("thermal", "unknown");
        for (int i = 0; i < 5; ++i) if (t == thermal_name(thermal_state(i))) p.thermal = thermal_state(i);
        p.source = "hook"; return p; });
    return b_->power();
}
result<connectivity_info> device::connectivity() {
    FORGE_SVC_NEED();
    if (auto s = b_->svc("connectivity", "{}")) return decode<connectivity_info>(s, [](const json::value& v) {
        connectivity_info c; c.online = v.find("online") && v["online"].b(); c.transport = v.str_or("transport", "unknown"); c.ssid = v.str_or("ssid", "");
        c.signal_percent = int(v.num_or("signal_percent", -1)); c.metered = v.find("metered") && v["metered"].b();
        if (auto* i = v.find("interfaces")) for (auto& x : i->arr()) c.interfaces.push_back(x.str());
        return c; });
    return b_->connectivity();
}
result<std::vector<ble_device>> device::bluetooth_scan(int ms) {
    FORGE_SVC_NEED(); if (ms <= 0 || ms > 120000) return {{}, {22, "bluetooth_scan: timeout_ms in (0, 120000]"}};
    if (auto s = b_->svc("bluetooth_scan", "{\"timeout_ms\":" + std::to_string(ms) + "}")) return decode<std::vector<ble_device>>(s, [](const json::value& v) {
        std::vector<ble_device> d; for (auto& x : v.arr()) d.push_back({x.str_or("address", ""), x.str_or("name", ""), int(x.num_or("rssi", 0))}); return d; });
    return b_->bluetooth_scan(ms);
}
result<bool> device::nfc_available() { FORGE_SVC_NEED(); if (auto s = b_->svc("nfc_available", "{}")) return decode<bool>(s, [](const json::value& v) { return v.b(); }); return b_->nfc_available(); }
result<std::string> device::clipboard_get() { FORGE_SVC_NEED(); if (auto s = b_->svc("clipboard_get", "{}")) return decode<std::string>(s, [](const json::value& v) { return v.str(); }); return b_->clipboard_get(); }
status device::clipboard_set(std::string_view t) { FORGE_SVC_NEED(); if (auto s = b_->svc("clipboard_set", "{\"text\":" + jstr(t) + "}")) return decode_status(s); return b_->clipboard_set(t); }
result<double> device::brightness() { FORGE_SVC_NEED(); if (auto s = b_->svc("brightness", "{}")) return decode<double>(s, [](const json::value& v) { return v.num(); }); return b_->brightness(); }
status device::set_brightness(double l) {
    FORGE_SVC_NEED(); if (!(l >= 0 && l <= 1)) return {{}, {22, "set_brightness: level in [0, 1]"}};
    if (auto s = b_->svc("set_brightness", "{\"level\":" + std::to_string(l) + "}")) return decode_status(s); return b_->set_brightness(l);
}
status device::keep_awake(bool on) { FORGE_SVC_NEED(); if (auto s = b_->svc("keep_awake", on ? "{\"on\":true}" : "{\"on\":false}")) return decode_status(s); return b_->keep_awake(on); }
status device::lock_orientation(orientation o) { FORGE_SVC_NEED(); if (auto s = b_->svc("lock_orientation", std::string("{\"orientation\":\"") + orientation_name(o) + "\"}")) return decode_status(s); return b_->lock_orientation(o); }
status device::show_keyboard(bool v) { FORGE_SVC_NEED(); if (auto s = b_->svc("show_keyboard", v ? "{\"show\":true}" : "{\"show\":false}")) return decode_status(s); return b_->show_keyboard(v); }
result<biometric_info> device::biometrics() {
    FORGE_SVC_NEED();
    if (auto s = b_->svc("biometrics", "{}")) return decode<biometric_info>(s, [](const json::value& v) {
        return biometric_info{v.find("available") && v["available"].b(), v.find("enrolled") && v["enrolled"].b(), v.str_or("kind", "")}; });
    return b_->biometrics();
}
result<device_info> device::info() {
    FORGE_SVC_NEED();
    if (auto s = b_->svc("info", "{}")) return decode<device_info>(s, [](const json::value& v) {
        return device_info{v.str_or("manufacturer", ""), v.str_or("model", ""), v.str_or("os_name", ""), v.str_or("os_version", ""), v.str_or("arch", ""), parse_form(v.str_or("form", "unknown"))}; });
    return b_->info();
}
result<storage_paths> device::paths() {
    FORGE_SVC_NEED();
    if (auto s = b_->svc("paths", "{}")) return decode<storage_paths>(s, [](const json::value& v) {
        return storage_paths{v.str_or("home", ""), v.str_or("documents", ""), v.str_or("downloads", ""), v.str_or("cache", ""), v.str_or("data", ""), v.str_or("temp", "")}; });
    return b_->paths();
}
status device::open_url(std::string_view u) { FORGE_SVC_NEED(); if (auto c = check_url(u); !c) return c; if (auto s = b_->svc("open_url", "{\"url\":" + jstr(u) + "}")) return decode_status(s); return b_->open_url(u); }
status device::share(std::string_view t) { FORGE_SVC_NEED(); if (auto s = b_->svc("share", "{\"text\":" + jstr(t) + "}")) return decode_status(s); return b_->share(t); }
status device::torch(bool on) { FORGE_SVC_NEED(); if (auto s = b_->svc("torch", on ? "{\"on\":true}" : "{\"on\":false}")) return decode_status(s); return b_->torch(on); }
result<std::vector<std::uint8_t>> device::capture_photo(std::string_view id) {
    FORGE_SVC_NEED();
    if (auto s = b_->svc("capture_photo", "{\"camera\":" + jstr(id) + "}")) return decode<std::vector<std::uint8_t>>(s, [](const json::value& v) {
        std::vector<std::uint8_t> b; for (auto& x : v["bytes"].arr()) b.push_back(std::uint8_t(x.i())); return b; });   // hooks return {"bytes":[...]}
    return b_->capture_photo(id);
}
result<audio_clip> device::record_audio(int ms, int rate) {
    FORGE_SVC_NEED(); if (ms <= 0 || ms > 600000 || rate < 8000 || rate > 192000) return {{}, {22, "record_audio: ms in (0, 600000], rate in [8000, 192000]"}};
    if (auto s = b_->svc("record_audio", "{\"ms\":" + std::to_string(ms) + ",\"sample_rate\":" + std::to_string(rate) + "}")) return decode<audio_clip>(s, [&](const json::value& v) {
        audio_clip c; c.sample_rate = int(v.num_or("sample_rate", rate)); c.channels = int(v.num_or("channels", 1));
        auto* x = v.find("samples");   // a hook that recorded nothing must say so, not have silence made up for it
        if (!x || !x->is_arr()) throw std::runtime_error("record_audio: hook returned no samples");
        for (auto& y : x->arr()) c.samples.push_back(std::int16_t(std::clamp<std::int64_t>(y.i(), -32768, 32767)));
        return c; });
    return b_->record_audio(ms, rate);
}
status device::play_audio(const audio_clip& c) {
    FORGE_SVC_NEED(); if (c.channels < 1 || c.sample_rate < 8000) return {{}, {22, "play_audio: bad clip"}};
    std::string a = "{\"sample_rate\":" + std::to_string(c.sample_rate) + ",\"channels\":" + std::to_string(c.channels) + ",\"samples\":[";
    for (std::size_t i = 0; i < c.samples.size(); ++i) { if (i) a += ','; a += std::to_string(c.samples[i]); }
    a += "]}";
    if (auto s = b_->svc("play_audio", a)) return decode_status(s);
    return b_->play_audio(c);
}
status device::haptic_pattern(const std::vector<int>& p, double amp) {
    FORGE_SVC_NEED(); if (p.empty() || p.size() > 64) return {{}, {22, "haptic_pattern: 1..64 on/off durations"}};
    for (int x : p) if (x <= 0 || x > 10000) return {{}, {22, "haptic_pattern: durations in (0, 10000] ms"}};
    std::string a = "{\"pattern\":["; for (std::size_t i = 0; i < p.size(); ++i) a += (i ? "," : "") + std::to_string(p[i]); a += "],\"amplitude\":" + std::to_string(amp) + "}";
    if (auto s = b_->svc("haptic_pattern", a)) return decode_status(s);
    // Native fallback: on/off through vibrate(). Hook and iOS vibrators return at once, evdev rumble blocks for the
    // duration: each step waits out whatever of its time the call did not take.
    for (std::size_t i = 0; i < p.size(); ++i) {
        auto t0 = std::chrono::steady_clock::now(), due = t0 + std::chrono::milliseconds(p[i]);
        if (i % 2 == 0) { if (auto r = vibrate(p[i], amp); !r) return r; }
        std::this_thread::sleep_until(due);
    }
    return {};
}
result<gnss_status> device::gnss() {
    FORGE_SVC_NEED();
    if (auto s = b_->svc("gnss", "{}")) return decode<gnss_status>(s, [](const json::value& v) {
        return gnss_status{v.find("fix") && v["fix"].b(), int(v.num_or("satellites_visible", -1)), int(v.num_or("satellites_used", -1))}; });
    return b_->gnss();
}
subscription device::on_lifecycle(std::function<void(app_event)> cb) {
    if (!b_ || !cb) return {};
    auto p = std::make_shared<std::function<void(app_event)>>(std::move(cb));
    bool first; { auto& br = bridge::get(); std::lock_guard g(br.m); first = br.life.empty(); br.life.push_back(p); }
    (void)first; b_->lifecycle_started();
    return subscription(p);
}
} // namespace forge::os::mobile
