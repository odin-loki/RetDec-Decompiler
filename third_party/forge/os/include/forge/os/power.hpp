#pragma once
// Power and thermal state: battery, AC, low-power mode, thermal pressure/throttling.
//   Linux: /sys/class/power_supply, /sys/firmware/acpi/platform_profile, /sys/class/thermal (temp vs passive trip),
//   cpufreq scaling_max < cpuinfo_max.  Android: same sysfs where readable, plus platform hooks that JNI glue calls
//   with PowerManager.isPowerSaveMode()/getCurrentThermalStatus() and BatteryManager.  Windows: GetSystemPowerStatus
//   (battery saver flag) + CallNtPowerInformation(ProcessorInformation) MHz limit.  macOS/iOS: NSProcessInfo
//   (lowPowerModeEnabled, thermalState) through the objc runtime; IOKit IOPS for the mac power source; UIDevice
//   battery on iOS.  FreeBSD: hw.acpi.battery / hw.acpi.acline / hw.acpi.thermal.
#include <forge/os/detail.hpp>
#if defined(FORGE_OS_IOS)
#include <atomic>
#include <condition_variable>
#endif

namespace forge::os {
enum class thermal_state { unknown, nominal, fair, serious, critical };
inline const char* thermal_name(thermal_state t) {
    switch (t) { case thermal_state::nominal: return "nominal"; case thermal_state::fair: return "fair"; case thermal_state::serious: return "serious"; case thermal_state::critical: return "critical"; default: return "unknown"; }
}
struct power_state {
    bool has_battery = false, on_battery = false, charging = false, low_power_mode = false, throttled = false;
    int battery_percent = -1;   // -1 unknown
    thermal_state thermal = thermal_state::unknown;
    double temperature_c = -1;  // hottest sensor we could read, -1 unknown
    std::string source;         // which backend filled this in
};
// Mobile glue (JNI on Android, app code on iOS) can push authoritative values here; they override probing.
struct power_hooks {
    static std::mutex& m() { static std::mutex x; return x; }
    static bool& has() { static bool b = false; return b; }
    static power_state& st() { static power_state s; return s; }
    static void report(const power_state& s) { std::lock_guard<std::mutex> g(m()); st() = s; has() = true; }
    static void clear() { std::lock_guard<std::mutex> g(m()); has() = false; }
};

inline power_state power_status() {
    power_state p;
    {
        std::lock_guard<std::mutex> g(power_hooks::m());
        if (power_hooks::has()) { p = power_hooks::st(); if (p.source.empty()) p.source = "hook"; return p; }
    }
#if defined(__linux__)
    p.source = "sysfs";
    // Several batteries (laptops with two packs): percent from summed energy (or charge), else the mean capacity.
    bool mains = false, discharging = false; long long now_sum = 0, full_sum = 0, cap_sum = 0; int caps = 0;
    if (auto l = list_dir("/sys/class/power_supply")) {
        for (auto& e : l.value) {
            std::string d = "/sys/class/power_supply/" + e.name, type = detail::trim(detail::slurp(d + "/type"));
            if (type == "Battery") {
                if (detail::slurp_ll(d + "/present", 1) == 0 || detail::trim(detail::slurp(d + "/scope")) == "Device") continue;   // empty bay; mouse/keyboard batteries
                p.has_battery = true;
                auto cap = detail::slurp_ll(d + "/capacity", -1); if (cap >= 0) { cap_sum += cap; ++caps; }
                auto en = detail::slurp_ll(d + "/energy_now", -1), ef = detail::slurp_ll(d + "/energy_full", -1);
                if (en < 0 || ef <= 0) { en = detail::slurp_ll(d + "/charge_now", -1); ef = detail::slurp_ll(d + "/charge_full", -1); }
                if (en >= 0 && ef > 0) { now_sum += en; full_sum += ef; }
                auto st = detail::trim(detail::slurp(d + "/status"));
                p.charging |= st == "Charging";   // "Full" and "Not charging" are not charging
                discharging |= st == "Discharging";
            } else if (type == "Mains" || type == "USB" || type == "USB_C" || type == "USB_PD") { if (detail::slurp_ll(d + "/online", 0) == 1) mains = true; }
        }
    }
    if (full_sum > 0) p.battery_percent = int(std::min<long long>(100, (now_sum * 100 + full_sum / 2) / full_sum));
    else if (caps) p.battery_percent = int(cap_sum / caps);
    p.on_battery = p.has_battery && !mains && discharging;
    auto prof = detail::trim(detail::slurp("/sys/firmware/acpi/platform_profile")); if (prof == "low-power" || prof == "quiet") p.low_power_mode = true;
    // Thermal state from each zone's own trip points: at/over critical or hot -> critical, over passive (where
    // the kernel starts throttling) -> serious, within 10 C of passive -> fair. Zones without trips only give a temperature.
    int worst = -1;
    if (auto l = list_dir("/sys/class/thermal")) {
        for (auto& e : l.value) {
            if (e.name.rfind("thermal_zone", 0)) continue;
            std::string d = "/sys/class/thermal/" + e.name; auto t = detail::slurp_ll(d + "/temp", -1); if (t <= 0 || t > 200000) continue;
            double c = double(t) / 1000.0; if (c > p.temperature_c) p.temperature_c = c;
            int z = 0;
            for (int i = 0; i < 16; ++i) {
                auto ty = detail::trim(detail::slurp(d + "/trip_point_" + std::to_string(i) + "_type")); if (ty.empty()) break;
                auto tt = detail::slurp_ll(d + "/trip_point_" + std::to_string(i) + "_temp", -1); if (tt <= 0 || tt > 200000) continue;
                if ((ty == "critical" || ty == "hot") && t >= tt) z = std::max(z, 3);
                else if (ty == "passive") z = std::max(z, t >= tt ? 2 : t >= tt - 10000 ? 1 : 0);
            }
            worst = std::max(worst, z);
        }
    }
    if (p.temperature_c >= 0) p.thermal = thermal_state(int(thermal_state::nominal) + std::max(worst, 0));
    // Throttled: the CPU's thermal-throttle counters moved since the last call (Intel/AMD with therm_throttle), or
    // a zone is past its passive trip. A lowered scaling_max_freq is a user / power-profile cap, not throttling.
    {
        static std::mutex tm; static long long last = -1;
        long long sum = -1;
        for (unsigned c = 0; c < cpu_count(); ++c) {
            auto v = detail::slurp_ll("/sys/devices/system/cpu/cpu" + std::to_string(c) + "/thermal_throttle/core_throttle_count", -1);
            auto w = detail::slurp_ll("/sys/devices/system/cpu/cpu" + std::to_string(c) + "/thermal_throttle/package_throttle_count", -1);
            if (v >= 0 || w >= 0) sum = std::max(sum, 0ll) + std::max(v, 0ll) + std::max(w, 0ll);
            if (v < 0 && w < 0 && c == 0) break;
        }
        std::lock_guard<std::mutex> g(tm);
        if (sum >= 0 && last >= 0 && sum > last) p.throttled = true;
        if (sum >= 0) last = sum;
    }
    if (p.thermal >= thermal_state::serious) p.throttled = true;
#elif defined(FORGE_OS_WINDOWS)
    p.source = "win32";
    SYSTEM_POWER_STATUS s{};
    if (GetSystemPowerStatus(&s)) {
        p.has_battery = s.BatteryFlag != 128 && s.BatteryFlag != 255; p.on_battery = s.ACLineStatus == 0;
        p.charging = (s.BatteryFlag & 8) != 0; if (s.BatteryLifePercent <= 100) p.battery_percent = s.BatteryLifePercent;
        p.low_power_mode = s.SystemStatusFlag == 1;   // battery saver
    }
    struct ppi { ULONG Number, MaxMhz, CurrentMhz, MhzLimit, MaxIdleState, CurrentIdleState; };
    if (HMODULE h = LoadLibraryExW(L"powrprof.dll", nullptr, 0x800 /*LOAD_LIBRARY_SEARCH_SYSTEM32*/)) {
        using fn = LONG(WINAPI*)(int, PVOID, ULONG, PVOID, ULONG);
        if (auto f = reinterpret_cast<fn>(reinterpret_cast<void*>(GetProcAddress(h, "CallNtPowerInformation")))) {
            std::vector<ppi> v(cpu_count());
            if (f(11 /*ProcessorInformation*/, nullptr, 0, v.data(), ULONG(v.size() * sizeof(ppi))) == 0 && !v.empty() && v[0].MaxMhz) {
                p.throttled = v[0].MhzLimit < v[0].MaxMhz * 9 / 10;
                p.thermal = v[0].MhzLimit < v[0].MaxMhz / 2 ? thermal_state::serious : p.throttled ? thermal_state::fair : thermal_state::nominal;
            }
        }
        FreeLibrary(h);
    }
#elif defined(__APPLE__)
    p.source = "foundation";
    auto& o = detail::objc_api::get();
    if (o.ok()) {
        if (void* pi = o.send(o.cls("NSProcessInfo"), "processInfo")) {
            p.low_power_mode = o.send<signed char>(pi, "isLowPowerModeEnabled") != 0;
            long ts = o.send<long>(pi, "thermalState");   // NSProcessInfoThermalState 0..3
            p.thermal = thermal_state(int(thermal_state::nominal) + int(ts < 0 ? 0 : ts > 3 ? 3 : ts));
            p.throttled = ts >= 2;
        }
    }
#if defined(FORGE_OS_IOS)
    // UIDevice is main-thread-only UIKit: read it there (directly on the main thread, else via an async hop that
    // is waited for up to 250 ms - a blocked main thread leaves the battery unknown instead of deadlocking).
    struct batt { std::mutex m; std::condition_variable cv; bool done = false; float lvl = -1; long st = 0; std::atomic<int> refs{2}; };
    auto read = [](void* c) {
        auto* b = static_cast<batt*>(c); auto& oo = detail::objc_api::get();
        float lvl = -1; long st = 0;
        if (oo.ok()) if (void* dev = oo.send(oo.cls("UIDevice"), "currentDevice")) {
            oo.send<void, signed char>(dev, "setBatteryMonitoringEnabled:", 1);
            lvl = reinterpret_cast<float (*)(void*, void*)>(oo.msg)(dev, oo.sel("batteryLevel")); st = oo.send<long>(dev, "batteryState");   // 1 unplugged, 2 charging, 3 full
        }
        { std::lock_guard<std::mutex> g(b->m); b->lvl = lvl; b->st = st; b->done = true; } b->cv.notify_all();
        if (--b->refs == 0) delete b;
    };
    auto* b = new batt;
    static auto main_np = reinterpret_cast<int (*)()>(::dlsym(RTLD_DEFAULT, "pthread_main_np"));
    static auto async_f = reinterpret_cast<void (*)(void*, void*, void (*)(void*))>(::dlsym(RTLD_DEFAULT, "dispatch_async_f"));
    static void* main_q = ::dlsym(RTLD_DEFAULT, "_dispatch_main_q");
    if (main_np && main_np()) read(b);
    else if (async_f && main_q) async_f(main_q, b, read);
    else --b->refs;
    {
        std::unique_lock<std::mutex> l(b->m); b->cv.wait_for(l, std::chrono::milliseconds(250), [&] { return b->done; });
        if (b->done && b->st) { p.has_battery = true; if (b->lvl >= 0) p.battery_percent = int(b->lvl * 100 + 0.5f); p.charging = b->st == 2; p.on_battery = b->st == 1; }
    }
    if (--b->refs == 0) delete b;
#else
    if (void* io = ::dlopen("/System/Library/Frameworks/IOKit.framework/IOKit", RTLD_LAZY)) {
        auto& cf = detail::cf_api::get();
        using copy_fn = detail::CFTypeRef (*)(); using type_fn = detail::CFStringRef (*)(detail::CFTypeRef);
        auto info = reinterpret_cast<copy_fn>(::dlsym(io, "IOPSCopyPowerSourcesInfo"));
        auto prov = reinterpret_cast<type_fn>(::dlsym(io, "IOPSGetProvidingPowerSourceType"));
        auto list = reinterpret_cast<detail::CFTypeRef (*)(detail::CFTypeRef)>(::dlsym(io, "IOPSCopyPowerSourcesList"));
        auto desc = reinterpret_cast<detail::CFDictionaryRef (*)(detail::CFTypeRef, detail::CFTypeRef)>(::dlsym(io, "IOPSGetPowerSourceDescription"));
        if (cf.ok && info && prov) if (auto blob = info()) {
            p.on_battery = cf.to_string(prov(blob)) == "Battery Power";
            if (list && desc) if (auto arr = list(blob)) {
                long long cur_sum = 0, max_sum = 0;   // every internal battery, summed
                for (detail::CFIndex i = 0; i < cf.CFArrayGetCount(arr); ++i) {
                    auto d = desc(blob, cf.CFArrayGetValueAtIndex(arr, i)); if (!d) continue;
                    auto kcur = cf.str("Current Capacity"), kmax = cf.str("Max Capacity"), kch = cf.str("Is Charging");
                    long long cur = cf.to_ll(cf.CFDictionaryGetValue(d, kcur)), mx = cf.to_ll(cf.CFDictionaryGetValue(d, kmax));
                    if (cur >= 0 && mx > 0) { p.has_battery = true; cur_sum += cur; max_sum += mx; }
                    p.charging |= cf.CFDictionaryGetValue(d, kch) == cf.kCFBooleanTrue;
                    cf.CFRelease(kcur); cf.CFRelease(kmax); cf.CFRelease(kch);
                }
                if (max_sum > 0) p.battery_percent = int(std::min<long long>(100, cur_sum * 100 / max_sum));
                cf.CFRelease(arr);
            }
            cf.CFRelease(blob);
        }
    }
#endif
#elif defined(FORGE_OS_FREEBSD)
    p.source = "acpi";
    long long life = detail::sysctl_ll("hw.acpi.battery.life", -1), ac = detail::sysctl_ll("hw.acpi.acline", -1), st = detail::sysctl_ll("hw.acpi.battery.state", -1);
    if (life >= 0) { p.has_battery = true; p.battery_percent = int(life); }
    if (ac >= 0) p.on_battery = ac == 0; if (st >= 0) p.charging = (st & 2) != 0;
    long long tz = detail::sysctl_ll("hw.acpi.thermal.tz0.temperature", -1), psv = detail::sysctl_ll("hw.acpi.thermal.tz0._PSV", -1);
    if (tz > 0) { p.temperature_c = double(tz) / 10.0 - 273.15; p.thermal = psv > 0 && tz >= psv ? thermal_state::serious : psv > 0 && tz >= psv - 100 ? thermal_state::fair : thermal_state::nominal; }
    p.throttled = p.thermal >= thermal_state::serious;
#endif
    return p;
}
} // namespace forge::os
