// forge_mobile FreeBSD backend.
//   sensors   temperature: sysctl dev.cpu.0.temperature / hw.acpi.thermal.tz0.temperature (deci-kelvin);
//             battery: hw.acpi.battery.life / hw.acpi.acline (via forge::os::power).
//   touch     evdev (/dev/input/event*, `kldload evdev` + hw.usb.usbhid.enable) multitouch protocol B and
//             single touch, every node with INPUT_PROP_DIRECT (touchscreens; touchpads are skipped).
//   display   not exposed without X/Wayland: app glue calls report_display().  Hot-plug arrives via devd(8);
//             apps can forward it with feed_lifecycle/feed_sensor.
//   brightness backlight(9) (FreeBSD 13+): /dev/backlight/backlight* with BACKLIGHTGETSTATUS / BACKLIGHTUPDATESTATUS
//             (percent). Writing needs access to the device node (root or a devfs rule).
//   keep_awake FreeBSD has no idle/sleep inhibitor service. Under X11 (DISPLAY set, xset installed) the screen saver
//             and DPMS are switched off with xset and the previous `xset q` settings restored on keep_awake(false)
//             or when the device is destroyed (a crash leaves them off until the X server restarts). Otherwise
//             unsupported; console and Wayland sessions have nothing to inhibit through.
#include "internal.hpp"
#if defined(__FreeBSD__)
#include "evdev.hpp"
#include <algorithm>
#include <cmath>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/sysctl.h>
#include <unistd.h>
#if __has_include(<sys/backlight.h>)
#include <sys/backlight.h>
#define FORGE_HAVE_BACKLIGHT 1
#endif

namespace forge::os::mobile::detail {
namespace {
bool temp_c(double& out) {
    for (const char* n : {"dev.cpu.0.temperature", "hw.acpi.thermal.tz0.temperature"}) {
        int dk = 0; std::size_t len = sizeof dk;
        if (::sysctlbyname(n, &dk, &len, nullptr, 0) == 0 && dk > 0) { out = dk / 10.0 - 273.15; return true; }
    }
    return false;
}
#if defined(FORGE_HAVE_BACKLIGHT)
// First backlight(9) node: backlight0, backlight1, ... (driver aliases such as intel_backlight0 point at the same).
std::string backlight_node() {
    std::vector<std::string> v;
    if (auto l = list_dir("/dev/backlight")) for (auto& e : l.value) if (e.name.rfind("backlight", 0) == 0) v.push_back(e.name);
    std::sort(v.begin(), v.end());
    return v.empty() ? std::string() : "/dev/backlight/" + v[0];
}
#endif
struct freebsd_backend final : backend {
    std::mutex awake_m; bool awake_on = false; xset_saver saved;
    ~freebsd_backend() override { if (awake_on) keep_awake(false); }
    std::string name() const override { return "freebsd"; }
    result<double> brightness() override {
#if defined(FORGE_HAVE_BACKLIGHT)
        auto n = backlight_node(); if (n.empty()) return {{}, unsupported("screen brightness (no /dev/backlight device)")};
        int fd = ::open(n.c_str(), O_RDONLY | O_CLOEXEC); if (fd < 0) return {{}, last_error("open " + n)};
        backlight_props p{}; int r = ::ioctl(fd, BACKLIGHTGETSTATUS, &p); auto e = r < 0 ? last_error("BACKLIGHTGETSTATUS") : error{}; ::close(fd);
        if (r < 0) return {{}, e};
        return {std::min(p.brightness, 100u) / 100.0, {}};
#else
        return {{}, unsupported("screen brightness (backlight(9) needs FreeBSD 13+)")};
#endif
    }
    status set_brightness(double l) override {
#if defined(FORGE_HAVE_BACKLIGHT)
        auto n = backlight_node(); if (n.empty()) return {{}, unsupported("screen brightness (no /dev/backlight device)")};
        int fd = ::open(n.c_str(), O_RDWR | O_CLOEXEC); if (fd < 0) return {{}, last_error("open " + n)};
        backlight_props p{}; int r = ::ioctl(fd, BACKLIGHTGETSTATUS, &p);
        if (r == 0) { p.brightness = std::uint32_t(std::lround(l * 100)); r = ::ioctl(fd, BACKLIGHTUPDATESTATUS, &p); }
        auto e = r < 0 ? last_error("backlight ioctl") : error{}; ::close(fd);
        return {{}, e};
#else
        (void)l; return {{}, unsupported("screen brightness (backlight(9) needs FreeBSD 13+)")};
#endif
    }
    status keep_awake(bool on) override {
        std::lock_guard g(awake_m);
        if (on == awake_on) return {};
        if (on) {
            if (os::getenv("DISPLAY").empty()) return {{}, unsupported("keep awake (FreeBSD has no idle inhibitor; only X11 via xset, and DISPLAY is not set)")};
            std::string out; int rc = run_tool({"xset", "q"}, &out, 5000);
            if (rc < 0) return {{}, unsupported("keep awake (no xset)")};
            if (rc != 0) return {{}, {rc, "xset q failed (X server not reachable?)"}};
            saved = parse_xset_q(out);
            if (run_tool({"xset", "s", "off"}) != 0) return {{}, {-1, "xset s off failed"}};
            if (saved.dpms == 1) run_tool({"xset", "-dpms"});
            awake_on = true; return {};
        }
        if (saved.timeout >= 0) run_tool({"xset", "s", std::to_string(saved.timeout), std::to_string(std::max(saved.cycle, 0))});
        else run_tool({"xset", "s", "default"});
        if (saved.dpms == 1) run_tool({"xset", "+dpms"});
        awake_on = false; return {};
    }
    result<std::vector<sensor_info>> sensors() override {
        std::vector<sensor_info> v; double t;
        if (temp_c(t)) v.push_back({sensor_kind::temperature, "sysctl thermal", "freebsd", 10, 0, 0.1});
        if (power_status().has_battery) v.push_back({sensor_kind::battery, "acpi battery", "freebsd", 1, 100, 1});
        return {v, {}};
    }
    result<sensor_stream> open_sensor(sensor_kind k, double hz) override {
        if (k != sensor_kind::temperature && k != sensor_kind::battery) return {{}, unsupported(std::string("sensor ") + sensor_name(k) + " on FreeBSD")};
        auto q = std::make_shared<queue>(); bridge::get().add_sensor(k, q);
        auto w = std::make_shared<worker>(); auto period = std::uint64_t(1e9 / hz);
        w->start([q, k, wp = w.get(), period] {
            do {
                sensor_event e; e.kind = k; e.t_ns = now_ns();
                if (k == sensor_kind::temperature) { if (temp_c(e.v[0])) { e.n = 1; q->push({false, e, {}}); } }
                else { auto p = power_status(); if (p.battery_percent >= 0) { e.v = {double(p.battery_percent), p.charging ? 1.0 : 0.0}; e.n = 2; q->push({false, e, {}}); } }
            } while (wp->sleep_ns(period));
        });
        return {make_sensor(k, q, w), {}};
    }
    // Every direct-touch evdev node (INPUT_PROP_DIRECT: touchscreens, not touchpads); positions in display pixels
    // once the app has reported the display size (report_display), else in device units.
    result<touch_stream> open_touch() override {
        int w_px = 0, h_px = 0; if (auto d = display()) { w_px = d.value.width_px; h_px = d.value.height_px; }
        std::vector<evdev_node> nodes;
        for (int i = 0; i < 64; ++i) {
            std::string p = "/dev/input/event" + std::to_string(i);
            int fd = ::open(p.c_str(), O_RDONLY | O_NONBLOCK | O_CLOEXEC); if (fd < 0) continue;
            constexpr int B = int(8 * sizeof(unsigned long));
            unsigned long bits[(ABS_CNT + B - 1) / B] = {}, keys[(KEY_CNT + B - 1) / B] = {};
            bool abs = ::ioctl(fd, EVIOCGBIT(EV_ABS, sizeof bits), bits) >= 0, key = ::ioctl(fd, EVIOCGBIT(EV_KEY, sizeof keys), keys) >= 0;
            auto bit = [](const unsigned long* b, int n) { return (b[n / B] >> (n % B)) & 1ul; };
            bool touch = abs && (bit(bits, ABS_MT_POSITION_X) || (bit(bits, ABS_X) && key && bit(keys, BTN_TOUCH)));
            if (touch && evdev_is_direct(fd)) nodes.push_back(evdev_setup(fd, w_px, h_px)); else ::close(fd);
        }
        if (nodes.empty()) return {{}, unsupported("touch input (no evdev touchscreen)")};
        auto q = std::make_shared<queue>(); bridge::get().add_touch(q); auto w = std::make_shared<worker>();
        w->start([nodes = std::move(nodes), q, wp = w.get()]() mutable { evdev_loop(std::move(nodes), *q, *wp); });
        return {make_touch(q, w), {}};
    }
    result<permission_state> permission_status(permission p) override { if (auto b = backend::permission_status(p)) return b; return {permission_state::not_required, {}}; }
    status request_permission(permission) override { return {}; }
};
} // namespace
std::shared_ptr<backend> make_freebsd() { return std::make_shared<freebsd_backend>(); }
} // namespace forge::os::mobile::detail
#endif
