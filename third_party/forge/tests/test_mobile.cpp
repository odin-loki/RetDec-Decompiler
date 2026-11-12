// forge::os::mobile: simulated backend, gesture recognizer, Linux IIO/evdev against fake trees.
#include <forge/os/mobile.hpp>
#include "../os/src/mobile/internal.hpp"   // pure helpers: remix, jpeg_with_dht, iio_channels, parse_xset_q
#include "check.hpp"
#include <cstdio>
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <mutex>
#include <thread>
#include <filesystem>
#include <fstream>
#if !defined(_WIN32)
#include <unistd.h>
#endif
#if defined(__linux__) && !defined(__ANDROID__)
#include <fcntl.h>
#include <linux/input.h>
#include <sys/stat.h>
#include <unistd.h>
#endif
using namespace forge::os;
namespace m = forge::os::mobile;
namespace fs = std::filesystem;

static const char* k_trace = R"({
 "display": {"width": 1080, "height": 2400, "dpi": 420, "refresh_hz": 120, "orientation": "portrait"},
 "sensors": ["accelerometer", {"kind": "gyroscope", "name": "sim-gyro"}],
 "cameras": [{"id": "0", "name": "back"}, {"id": "1", "name": "selfie", "front": true}],
 "permissions": {"camera": "denied", "notifications": "not_determined"},
 "events": [
  {"t_ms": 0, "type": "sensor", "kind": "accelerometer", "v": [0.1, 9.8, 0.2]},
  {"t_ms": 10, "type": "sensor", "kind": "accelerometer", "v": [0.2, 9.7, 0.1]},
  {"t_ms": 12, "type": "sensor", "kind": "gyroscope", "v": [0.01, 0.02, 0.03]},
  {"t_ms": 20, "type": "touch", "phase": "down", "id": 0, "x": 100, "y": 100},
  {"t_ms": 80, "type": "touch", "phase": "up", "id": 0, "x": 102, "y": 101},
  {"t_ms": 200, "type": "touch", "phase": "down", "id": 0, "x": 100, "y": 500},
  {"t_ms": 300, "type": "touch", "phase": "up", "id": 0, "x": 400, "y": 510},
  {"t_ms": 400, "type": "orientation", "value": "landscape"}
 ]})";

TEST(sim_device_basics) {
    auto d = m::device::simulated(k_trace); CHECK(d); if (!d) return;
    CHECK(d->backend_name() == "sim");
    CHECK(d->has_sensor(m::sensor_kind::accelerometer)); CHECK(!d->has_sensor(m::sensor_kind::light));
    auto disp = d->display(); CHECK(disp); CHECK(disp->orient == m::orientation::landscape_left); CHECK(disp->width_px == 2400);
    auto cams = d->cameras(); CHECK(cams && cams->size() == 2 && cams->at(1).front);
    CHECK(d->permission_status(m::permission::camera).value == m::permission_state::denied);
    CHECK(d->notify("t", "b"));
    CHECK(d->request_permission(m::permission::notifications));
    CHECK(d->permission_status(m::permission::notifications).value == m::permission_state::granted);
    CHECK(d->vibrate(40, 0.5)); CHECK(!d->vibrate(0));
    auto log = d->sim_log(); CHECK(log.size() == 3);
    auto light = d->open_sensor(m::sensor_kind::light); CHECK(!light && is_unsupported(light.err));
}
TEST(sim_sensor_stream) {
    auto d = m::device::simulated(k_trace); if (!d) { CHECK(false); return; }
    auto s = d->open_sensor(m::sensor_kind::accelerometer, 100); CHECK(s); if (!s) return;
    std::vector<m::sensor_event> all;
    for (int i = 0; i < 50 && !s->finished(); ++i) for (auto& e : s->poll(20)) all.push_back(e);
    CHECK(all.size() == 2); if (all.size() == 2) { CHECK(all[0].n == 3); CHECK(all[1].v[1] == 9.7); }
}
TEST(sim_touch_and_gestures) {
    auto d = m::device::simulated(k_trace); if (!d) { CHECK(false); return; }
    auto t = d->open_touch(); CHECK(t); if (!t) return;
    std::vector<m::gesture> g;
    for (int i = 0; i < 50 && !t->finished(); ++i) for (auto& x : t->gestures(20)) g.push_back(x);
    CHECK(g.size() == 2);
    if (g.size() == 2) { CHECK(g[0].kind == m::gesture_kind::tap); CHECK(g[1].kind == m::gesture_kind::swipe); CHECK(g[1].dx == 300); }
}
TEST(gesture_pinch_double_long) {
    m::gesture_recognizer r; std::vector<m::gesture> g;
    auto f = [&](std::uint64_t ms, m::touch_phase p, int id, double x, double y) { for (auto& e : r.feed({ms * 1000000, p, id, x, y})) g.push_back(e); };
    f(0, m::touch_phase::down, 0, 10, 10); f(50, m::touch_phase::up, 0, 10, 10);
    f(150, m::touch_phase::down, 0, 12, 10); f(200, m::touch_phase::up, 0, 12, 10);
    CHECK(g.size() == 3 && g[2].kind == m::gesture_kind::double_tap); g.clear();
    f(1000, m::touch_phase::down, 0, 50, 50); f(1700, m::touch_phase::move, 0, 52, 50); f(1800, m::touch_phase::up, 0, 52, 50);
    CHECK(g.size() == 1 && g[0].kind == m::gesture_kind::long_press); g.clear();
    f(3000, m::touch_phase::down, 0, 100, 100); f(3001, m::touch_phase::down, 1, 200, 100);
    f(3050, m::touch_phase::move, 1, 300, 100);
    f(3100, m::touch_phase::up, 0, 100, 100); f(3100, m::touch_phase::up, 1, 300, 100);
    CHECK(!g.empty() && g.back().kind == m::gesture_kind::pinch && g.back().scale == 2.0);
}
TEST(sim_bad_trace) {
    CHECK(!m::device::simulated("{\"events\":[{\"type\":\"warp\"}]}"));
    CHECK(!m::device::simulated("not json"));
}
TEST(bridge_feed) {
    auto d = m::device::simulated("{}"); if (!d) { CHECK(false); return; }
    auto t = d->open_touch(); if (!t) { CHECK(false); return; }
    for (int i = 0; i < 20 && !t->finished(); ++i) t->poll(5);
    m::feed_touch({1, m::touch_phase::down, 3, 1, 2});
    auto v = t->poll(0); CHECK(v.size() == 1 && v[0].id == 3);
}
TEST(null_device_unsupported) {
    auto d = m::device::null_device(); CHECK(d);
    auto s = d->open_sensor(m::sensor_kind::accelerometer); CHECK(!s && is_unsupported(s.err));
    CHECK(is_unsupported(d->open_touch().err));
    CHECK(is_unsupported(d->cameras().err));
}
TEST(platform_open) { auto d = m::device::open(); CHECK(d); if (d) CHECK(!d->backend_name().empty()); }

static const char* k_svc = R"({"permissions": {"camera": "denied"},
 "services": {"power": {"battery_percent": 64, "charging": true}, "connectivity": {"online": true, "transport": "wifi", "ssid": "home", "signal_percent": 70},
  "bluetooth_scan": [{"address": "AA:BB", "name": "band", "rssi": -60}], "clipboard": "hi", "brightness": 0.3,
  "info": {"manufacturer": "Sim", "model": "S1", "form": "watch"}, "gnss": {"fix": true, "satellites_visible": 9, "satellites_used": 6}},
 "events": [{"t_ms": 0, "type": "lifecycle", "value": "background"}, {"t_ms": 5, "type": "lifecycle", "value": "foreground"}]})";
TEST(sim_services) {
    auto d = m::device::simulated(k_svc); CHECK(d); if (!d) return;
    auto p = d->power(); CHECK(p && p->battery_percent == 64);
    auto c = d->connectivity(); CHECK(c && c->online && c->transport == "wifi" && c->ssid == "home");
    auto b = d->bluetooth_scan(10); CHECK(b && b->size() == 1 && b->at(0).rssi == -60);
    auto cl = d->clipboard_get(); CHECK(cl && *cl == "hi"); CHECK(d->clipboard_set("yo")); CHECK(d->clipboard_get().value == "yo");
    CHECK(d->set_brightness(0.8)); auto br = d->brightness(); CHECK(br && *br > 0.79);
    CHECK(!d->torch(true));                       // camera permission denied
    auto g = d->gnss(); CHECK(g && g->fix && g->satellites_used == 6);
    auto nfc = d->nfc_available(); CHECK(!nfc && is_unsupported(nfc.err));   // absent from trace
    CHECK(d->keep_awake(true)); CHECK(d->open_url("https://example.com"));
    CHECK(d->haptic_pattern({20, 10, 20}));
}
TEST(sim_lifecycle) {
    auto d = m::device::simulated(k_svc); if (!d) { CHECK(false); return; }
    std::mutex mu; std::vector<m::app_event> got;
    auto sub = d->on_lifecycle([&](m::app_event e) { std::lock_guard g(mu); got.push_back(e); });
    m::feed_lifecycle(m::app_event::low_memory);
    for (int i = 0; i < 100; ++i) { { std::lock_guard g(mu); if (got.size() >= 3) break; } std::this_thread::sleep_for(std::chrono::milliseconds(5)); }
    std::lock_guard g(mu); CHECK(got.size() >= 3);
    CHECK(std::find(got.begin(), got.end(), m::app_event::low_memory) != got.end());
    CHECK(std::find(got.begin(), got.end(), m::app_event::background) != got.end());
}
TEST(open_url_policy) {
    auto d = m::device::simulated(k_svc); CHECK(d); if (!d) return;
    for (const char* ok : {"https://example.com", "HTTP://example.com/a?b=c", "mailto:a@example.com", "tel:+15551234"}) CHECK_MSG(d->open_url(ok), "%s", ok);
    for (const char* bad : {"file:///etc/passwd", "/bin/sh", "C:\\Windows\\notepad.exe", "-oProxyCommand=x", "javascript:alert(1)",
                            "https://", "https:///x", "http:example.com", "https://a b", "https://a\x1b]8;;", "ms-settings:", "", "x"})
        CHECK_MSG(!d->open_url(bad), "%s", bad);
    m::set_url_schemes({"https", "MyApp"});
    CHECK(d->open_url("myapp:open?id=1")); CHECK(!d->open_url("http://example.com")); CHECK(d->open_url("https://example.com"));
    m::set_url_schemes({"http", "https", "mailto", "tel"});
    auto log = d->sim_log(); int opened = 0; for (auto& r : log) if (r.what == "open_url") ++opened;
    CHECK(opened == 6);   // refused URLs never reach the hook / opener
}
TEST(bridge_only_sensors) {
    auto d = m::device::null_device(); CHECK(d); if (!d) return;
    CHECK(is_unsupported(d->open_sensor(m::sensor_kind::heart_rate).err));
    m::provide_sensor(m::sensor_kind::heart_rate);
    auto s = d->open_sensor(m::sensor_kind::heart_rate); CHECK(s);
    auto list = d->sensors(); CHECK(list && list->size() == 1 && list->at(0).kind == m::sensor_kind::heart_rate);
    if (s) {
        m::sensor_event e; e.kind = m::sensor_kind::heart_rate; e.v[0] = 72; e.n = 1; m::feed_sensor(e);
        auto v = s->poll(100); CHECK(v.size() == 1 && v[0].v[0] == 72);
    }
    m::provide_sensor(m::sensor_kind::heart_rate, false);
    CHECK(is_unsupported(d->open_sensor(m::sensor_kind::heart_rate).err));
}
TEST(audio_and_haptics_hooks) {
    auto d = m::device::null_device(); CHECK(d); if (!d) return;
    std::string played; std::mutex mu;
    m::set_service_hook([&](std::string_view op, std::string_view args) -> std::optional<std::string> {
        if (op == "play_audio") { std::lock_guard g(mu); played = std::string(args); return std::string("true"); }
        if (op == "record_audio") return std::string("{\"sample_rate\":8000}");   // recorded nothing
        return std::nullopt;
    });
    m::audio_clip c; c.sample_rate = 8000; c.samples = {1, -2, 300};
    CHECK(d->play_audio(c));
    { std::lock_guard g(mu); CHECK(played.find("\"samples\":[1,-2,300]") != std::string::npos); }
    CHECK(!d->record_audio(100, 8000));                    // no samples: an error, not invented silence
    m::set_service_hook(nullptr);
    auto sim = m::device::simulated("{}"); if (sim) CHECK(!sim->record_audio(50));
    // haptic_pattern waits out each "on" even when the vibrator returns at once (hook, iOS)
    m::set_action_hooks({[](int, double) { return true; }, nullptr, nullptr});
    auto t0 = std::chrono::steady_clock::now();
    CHECK(d->haptic_pattern({40, 20, 40}));
    CHECK(std::chrono::steady_clock::now() - t0 >= std::chrono::milliseconds(95));
    m::set_action_hooks({});
}
#if !defined(_WIN32) && !defined(__APPLE__) && !defined(__ANDROID__)
TEST(xdg_user_dirs) {
    auto root = fs::temp_directory_path() / ("forge-xdg-" + std::to_string(::getpid()));
    fs::create_directories(root / "cfg");
    std::ofstream(root / "cfg/user-dirs.dirs") << "# comment\nXDG_DOCUMENTS_DIR=\"$HOME/Papers\"\nXDG_DOWNLOAD_DIR=\"/data/dl\"\n";
    const char* oh = std::getenv("HOME"); const char* oc = std::getenv("XDG_CONFIG_HOME"); std::string sh = oh ? oh : "", sc = oc ? oc : "";
    forge_test::set_env("HOME", root.c_str()); forge_test::set_env("XDG_CONFIG_HOME", (root / "cfg").c_str());
    auto d = m::device::null_device(); auto p = d ? d->paths() : decltype(d->paths()){};
    CHECK(p && p->documents == (root / "Papers").string() && p->downloads == "/data/dl");
    forge_test::set_env("HOME", oh ? sh.c_str() : nullptr); forge_test::set_env("XDG_CONFIG_HOME", oc ? sc.c_str() : nullptr);
    fs::remove_all(root);
}
#endif
TEST(geofencing) {
    CHECK(std::abs(m::geofence_monitor::distance_m(0, 0, 0, 1) - 111195) < 200);
    m::geofence_monitor gm; gm.add({"home", -33.87, 151.21, 100});
    m::sensor_event away; away.kind = m::sensor_kind::location; away.v = {-33.90, 151.21}; away.n = 2;
    m::sensor_event inside = away; inside.v = {-33.8701, 151.2101};
    CHECK(gm.feed(away).empty());
    auto e = gm.feed(inside); CHECK(e.size() == 1 && e[0].entered);
    e = gm.feed(away); CHECK(e.size() == 1 && !e[0].entered);
    CHECK(gm.remove("home")); CHECK(!gm.remove("home"));
}
TEST(gesture_rotate) {
    m::gesture_recognizer r; std::vector<m::gesture> out;
    auto f = [&](std::uint64_t t, m::touch_phase ph, int id, double x, double y) { for (auto& g : r.feed({t, ph, id, x, y, 1, 1})) out.push_back(g); };
    f(0, m::touch_phase::down, 0, 100, 100); f(1, m::touch_phase::down, 1, 200, 100);
    f(20000000, m::touch_phase::move, 1, 100 + 100 * std::cos(0.5), 100 + 100 * std::sin(0.5));
    f(40000000, m::touch_phase::up, 1, 100 + 100 * std::cos(0.5), 100 + 100 * std::sin(0.5)); f(41000000, m::touch_phase::up, 0, 100, 100);
    bool rot = false; for (auto& g : out) if (g.kind == m::gesture_kind::rotate && std::abs(g.rotation) > 0.3) rot = true;
    CHECK(rot);
}
#if defined(__linux__) && !defined(__ANDROID__)
static void put(const fs::path& p, const std::string& s) { fs::create_directories(p.parent_path()); std::ofstream(p) << s << "\n"; }
TEST(linux_iio_fake_sysfs) {
    fs::path root = fs::temp_directory_path() / ("forge-mobile-" + std::to_string(getpid()));
    fs::remove_all(root);
    auto dev = root / "sys/bus/iio/devices/iio:device0";
    put(dev / "name", "bmi260"); put(dev / "in_accel_x_raw", "100"); put(dev / "in_accel_y_raw", "-2000"); put(dev / "in_accel_z_raw", "50");
    put(dev / "in_accel_scale", "0.0047");
    auto dev1 = root / "sys/bus/iio/devices/iio:device1"; put(dev1 / "name", "stk3310"); put(dev1 / "in_illuminance_input", "321.5");
    put(dev1 / "in_proximity_raw", "7");
    auto bat = root / "sys/class/power_supply/battery"; put(bat / "type", "Battery"); put(bat / "capacity", "77"); put(bat / "status", "Charging");
    put(root / "sys/class/drm/card0-DSI-1/status", "connected"); put(root / "sys/class/drm/card0-DSI-1/modes", "720x1440");
    put(root / "sys/class/video4linux/video0/name", "ov5640 front");
    put(root / "sys/class/input/event0/device/capabilities/abs", "260800000000000");   // ABS_MT_POSITION_X/Y, TRACKING_ID, SLOT
    put(root / "sys/class/input/event0/device/properties", "2");                       // INPUT_PROP_DIRECT: a touchscreen
    put(root / "sys/class/input/event1/device/capabilities/abs", "260800000000000");   // same axes, but a touchpad:
    put(root / "sys/class/input/event1/device/properties", "5");                       // INPUT_PROP_POINTER | BUTTONPAD
    put(root / "sys/class/video4linux/video1/name", "ov5640 front"); put(root / "sys/class/video4linux/video1/index", "1");   // UVC metadata node
    put(root / "sys/class/video4linux/video0/index", "0");
    put(root / "sys/firmware/devicetree/base/model", std::string("Raspberry Pi 4 Model B") + char(0));
    fs::create_directories(root / "input");
    auto d = m::device::linux_with((root / "sys").string(), (root / "input").string()); CHECK(d); if (!d) return;
    auto ss = d->sensors(); CHECK(ss && ss->size() == 4);
    auto a = d->open_sensor(m::sensor_kind::accelerometer, 200); CHECK(a);
    if (a) { auto v = a->poll(500, 1); CHECK(v.size() == 1); if (!v.empty()) { CHECK(std::abs(v[0].v[1] + 9.4) < 1e-9); } }
    auto l = d->open_sensor(m::sensor_kind::light, 100); if (l) { auto v = l->poll(500, 1); CHECK(!v.empty() && v[0].v[0] == 321.5); } else CHECK(false);
    auto b = d->open_sensor(m::sensor_kind::battery, 100); if (b) { auto v = b->poll(500, 1); CHECK(!v.empty() && v[0].v[0] == 77 && v[0].v[1] == 1); } else CHECK(false);
    auto disp = d->display(); CHECK(disp && disp->width_px == 720 && disp->orient == m::orientation::portrait_upside_down);
    auto cams = d->cameras(); CHECK(cams && cams->size() == 1 && cams->at(0).front);
    CHECK(d->permission_status(m::permission::camera).value == m::permission_state::not_required);
    CHECK(is_unsupported(d->open_sensor(m::sensor_kind::location).err));
    auto inf = d->info(); CHECK(inf && inf->model == "Raspberry Pi 4 Model B");
    if (inf) CHECK(inf->form != m::form_factor::desktop);   // battery + touchscreen here: phone; never desktop from a DT model alone

    // evdev multitouch protocol B through a FIFO standing in for /dev/input/event0
    auto node = root / "input/event0"; CHECK(::mkfifo(node.c_str(), 0600) == 0);
    auto t = d->open_touch(); CHECK(t);
    if (t) {
        int w = -1; for (int i = 0; i < 100 && w < 0; ++i) { w = ::open(node.c_str(), O_WRONLY | O_NONBLOCK); if (w < 0) ::usleep(10000); }
        CHECK(w >= 0);
        auto ev = [&](int type, int code, int val, int ms) { input_event e{}; e.input_event_sec = 0; e.input_event_usec = ms * 1000; e.type = std::uint16_t(type); e.code = std::uint16_t(code); e.value = val; (void)!::write(w, &e, sizeof e); };
        ev(EV_ABS, ABS_MT_SLOT, 0, 0); ev(EV_ABS, ABS_MT_TRACKING_ID, 5, 0); ev(EV_ABS, ABS_MT_POSITION_X, 10, 0); ev(EV_ABS, ABS_MT_POSITION_Y, 20, 0); ev(EV_SYN, SYN_REPORT, 0, 0);
        ev(EV_ABS, ABS_MT_SLOT, 1, 10); ev(EV_ABS, ABS_MT_TRACKING_ID, 6, 10); ev(EV_ABS, ABS_MT_POSITION_X, 30, 10); ev(EV_ABS, ABS_MT_POSITION_Y, 40, 10); ev(EV_SYN, SYN_REPORT, 0, 10);
        ev(EV_ABS, ABS_MT_SLOT, 0, 20); ev(EV_ABS, ABS_MT_TRACKING_ID, -1, 20); ev(EV_SYN, SYN_REPORT, 0, 20);
        ::close(w);
        std::vector<m::touch_event> all; for (int i = 0; i < 100 && !t->finished(); ++i) for (auto& e : t->poll(20)) all.push_back(e);
        CHECK(all.size() == 3);
        if (all.size() == 3) {
            CHECK(all[0].phase == m::touch_phase::down && all[0].id == 5 && all[0].x == 10 && all[0].y == 20);
            CHECK(all[1].phase == m::touch_phase::down && all[1].id == 6 && all[1].pointers == 2);
            CHECK(all[2].phase == m::touch_phase::up && all[2].id == 5 && all[2].t_ns == 20000000);
        }
    }
    fs::remove_all(root);
}
#endif

// ---- pure helpers shared by the native backends
TEST(helper_remix) {
    namespace d = m::detail;
    float mono[] = {0.5f, -0.5f};
    auto st = d::remix(mono, 2, 1, 8000, 2, 8000); CHECK(st.size() == 4 && st[0] == 0.5f && st[1] == 0.5f && st[3] == -0.5f);
    float lr[] = {1.0f, 0.0f, 0.2f, 0.4f};
    auto mo = d::remix(lr, 2, 2, 8000, 1, 8000); CHECK(mo.size() == 2 && mo[0] == 0.5f && std::abs(mo[1] - 0.3f) < 1e-6f);
    float ramp[] = {0.0f, 1.0f, 2.0f, 3.0f};
    auto up = d::remix(ramp, 4, 1, 8000, 1, 16000); CHECK(up.size() == 8);
    if (up.size() == 8) { CHECK(up[1] == 0.5f); CHECK(up[2] == 1.0f); CHECK(up[7] == 3.0f); }
    std::vector<float> s48(4800, 0.25f);
    auto down = d::remix(s48.data(), 4800, 1, 48000, 1, 16000); CHECK(down.size() == 1600 && down[800] == 0.25f);
    CHECK(d::remix(nullptr, 4, 1, 8000, 1, 8000).empty()); CHECK(d::remix(ramp, 4, 0, 8000, 1, 8000).empty());
    float six[] = {0.1f, 0.2f, 0.3f, 0.4f, 0.5f, 0.6f};
    auto two = d::remix(six, 1, 6, 48000, 2, 48000); CHECK(two.size() == 2 && two[0] == 0.1f && two[1] == 0.2f);
    auto four = d::remix(lr, 1, 2, 48000, 4, 48000); CHECK(four.size() == 4 && four[0] == 1.0f && four[2] == 0.0f);
}
TEST(helper_jpeg_dht) {
    namespace d = m::detail;
    // SOI, APP0 (len 4), SOF0 (len 5), SOS (len 3) + entropy data, EOI: the shape of a UVC MJPEG frame.
    std::vector<std::uint8_t> f = {0xFF, 0xD8, 0xFF, 0xE0, 0x00, 0x04, 'J', 'F', 0xFF, 0xC0, 0x00, 0x05, 8, 0, 1,
                                   0xFF, 0xDA, 0x00, 0x03, 0x00, 0x12, 0x34, 0xFF, 0xD9};
    auto g = d::jpeg_with_dht(f);
    CHECK(g.size() == f.size() + 420);
    if (g.size() == f.size() + 420) {
        CHECK(g[15] == 0xFF && g[16] == 0xC4 && g[17] == 0x01 && g[18] == 0xA2);   // DHT, length 418, right before SOS
        CHECK(g[19] == 0x00 && g[20 + 16 + 12] == 0x10 && g[20 + 16 + 12 + 1 + 16 + 162] == 0x01);   // DC0, AC0, DC1 table ids
        std::size_t ac1 = 20 + 16 + 12 + 1 + 16 + 162 + 1 + 16 + 12; CHECK(g[ac1] == 0x11);
        int sum = 0; for (int i = 0; i < 16; ++i) sum += g[ac1 + 1 + std::size_t(i)]; CHECK(sum == 162);
        CHECK(g[435] == 0xFF && g[436] == 0xDA && g.back() == 0xD9);
    }
    CHECK(d::jpeg_with_dht(g) == g);                                   // already has tables
    std::vector<std::uint8_t> junk = {1, 2, 3, 4, 5}; CHECK(d::jpeg_with_dht(junk) == junk);
    std::vector<std::uint8_t> trunc = {0xFF, 0xD8, 0xFF, 0xE0, 0x10, 0x00}; CHECK(d::jpeg_with_dht(trunc) == trunc);
}
TEST(helper_iio_channels) {
    namespace d = m::detail;
    auto ch = d::iio_channels({"name", "in_accel_x_raw", "in_accel_y_raw", "in_accel_z_raw", "in_accel_scale", "in_magn_x_raw", "in_magn_y_raw",   // magn lacks z
                               "in_illuminance0_raw", "in_illuminance_ir_raw", "in_illuminance_scale", "in_temp_object_raw", "in_temp_ambient_raw",
                               "in_pressure_input", "in_humidityrelative_input", "in_proximity0_raw", "in_rot_quaternion_raw", "in_steps_input",
                               "in_anglvel_x_raw", "in_anglvel_y_raw", "in_anglvel_z_raw", "in_intensity_both_raw", "out_voltage0_raw", "in_accel_x_calibbias"});
    auto find = [&](m::sensor_kind k) -> const d::iio_channel* { for (auto& c : ch) if (c.kind == k) return &c; return nullptr; };
    auto* a = find(m::sensor_kind::accelerometer); CHECK(a && a->prefix == "in_accel" && a->axes == 3);
    CHECK(!find(m::sensor_kind::magnetometer));
    auto* l = find(m::sensor_kind::light); CHECK(l && l->prefix == "in_illuminance0" && l->type_prefix == "in_illuminance");
    auto* t = find(m::sensor_kind::temperature); CHECK(t && t->prefix == "in_temp_ambient" && std::abs(t->mul - 0.001) < 1e-12);
    auto* p = find(m::sensor_kind::pressure); CHECK(p && p->prefix == "in_pressure" && p->mul == 10.0);
    CHECK(find(m::sensor_kind::humidity)); CHECK(find(m::sensor_kind::proximity)); CHECK(find(m::sensor_kind::step_counter)); CHECK(find(m::sensor_kind::gyroscope));
    auto* r = find(m::sensor_kind::rotation_vector); CHECK(r && r->prefix == "in_rot_quaternion" && r->axes == 4);
    CHECK(ch.size() == 9);
    CHECK(d::iio_channels({"in_temp_raw", "in_temp1_raw"}).at(0).prefix == "in_temp");
}
TEST(helper_xset_q) {
    namespace d = m::detail;
    const char* q = "Keyboard Control:\n  auto repeat:  on    key click percent:  0\nScreen Saver:\n  prefer blanking:  yes    allow exposures:  yes\n"
                    "  timeout:  600    cycle:  300\nDPMS (Energy Star):\n  Standby: 600    Suspend: 600    Off: 600\n  DPMS is Enabled\n  Monitor is On\n";
    auto s = d::parse_xset_q(q); CHECK(s.timeout == 600 && s.cycle == 300 && s.dpms == 1);
    auto n = d::parse_xset_q("Screen Saver:\n  timeout:  0    cycle:  600\nServer does not have the DPMS Extension\n"); CHECK(n.timeout == 0 && n.cycle == 600 && n.dpms == -1);
    auto e = d::parse_xset_q(""); CHECK(e.timeout == -1 && e.cycle == -1 && e.dpms == -1);
    CHECK(d::parse_xset_q("  DPMS is Disabled").dpms == 0);
}

#if defined(__linux__) && !defined(__ANDROID__)
// More IIO channel shapes, backlight choice and the paths that need real devices or D-Bus (fake trees never use them).
TEST(linux_iio_channels_fake_sysfs) {
    fs::path root = fs::temp_directory_path() / ("forge-mobile2-" + std::to_string(getpid()));
    fs::remove_all(root);
    auto d0 = root / "sys/bus/iio/devices/iio:device0";
    put(d0 / "name", "lis3mdl"); put(d0 / "in_magn_x_raw", "1000"); put(d0 / "in_magn_y_raw", "-500"); put(d0 / "in_magn_z_raw", "0"); put(d0 / "in_magn_scale", "0.000146");
    auto d1 = root / "sys/bus/iio/devices/iio:device1";
    put(d1 / "name", "bme280"); put(d1 / "in_pressure_input", "101.325"); put(d1 / "in_humidityrelative_input", "45000");
    put(d1 / "in_temp_object_raw", "9999"); put(d1 / "in_temp_ambient_raw", "2150"); put(d1 / "in_temp_ambient_offset", "0"); put(d1 / "in_temp_scale", "10");
    auto d2 = root / "sys/bus/iio/devices/iio:device2";
    put(d2 / "name", "als"); put(d2 / "in_illuminance0_raw", "200"); put(d2 / "in_illuminance_scale", "0.5"); put(d2 / "in_rot_quaternion_raw", "1 2 3 4"); put(d2 / "in_rot_scale", "0.25");
    put(root / "sys/class/backlight/acpi_video0/type", "firmware"); put(root / "sys/class/backlight/acpi_video0/brightness", "3"); put(root / "sys/class/backlight/acpi_video0/max_brightness", "10");
    put(root / "sys/class/backlight/a_raw/type", "raw"); put(root / "sys/class/backlight/a_raw/brightness", "100"); put(root / "sys/class/backlight/a_raw/max_brightness", "100");
    put(root / "sys/class/video4linux/video0/name", "Integrated Camera"); put(root / "sys/class/video4linux/video0/index", "0");
    fs::create_directories(root / "dev/input");
    auto d = m::device::linux_with((root / "sys").string(), (root / "dev/input").string()); CHECK(d); if (!d) return;
    auto ss = d->sensors(); CHECK(ss && ss->size() == 6);   // magn, pressure, humidity, temp, light, rotation; no location for fake trees
    auto first = [&](m::sensor_kind k) { m::sensor_event e{}; auto s = d->open_sensor(k, 200); if (!s) return e; auto v = s->poll(500, 1); if (!v.empty()) e = v[0]; return e; };
    auto mg = first(m::sensor_kind::magnetometer); CHECK(mg.n == 3 && std::abs(mg.v[0] - 14.6) < 1e-9 && std::abs(mg.v[1] + 7.3) < 1e-9);
    auto pr = first(m::sensor_kind::pressure); CHECK(pr.n == 1 && std::abs(pr.v[0] - 1013.25) < 1e-9);
    auto hu = first(m::sensor_kind::humidity); CHECK(hu.n == 1 && std::abs(hu.v[0] - 45.0) < 1e-9);
    auto te = first(m::sensor_kind::temperature); CHECK(te.n == 1 && std::abs(te.v[0] - 21.5) < 1e-9);   // ambient, not object; type-level scale
    auto li = first(m::sensor_kind::light); CHECK(li.n == 1 && li.v[0] == 100.0);
    auto ro = first(m::sensor_kind::rotation_vector); CHECK(ro.n == 4 && ro.v[0] == 0.25 && ro.v[3] == 1.0);
    auto b = d->brightness(); CHECK(b && std::abs(*b - 0.3) < 1e-12);   // firmware backlight beats raw
    CHECK(d->set_brightness(0.5)); CHECK(d->brightness().value == 0.5);
    CHECK(is_unsupported(d->open_sensor(m::sensor_kind::location).err));
    CHECK(is_unsupported(d->keep_awake(true).err)); CHECK(d->keep_awake(false));
    auto cams = d->cameras(); CHECK(cams && cams->size() == 1);
    auto ph = d->capture_photo(); CHECK(!ph && is_unsupported(ph.err));   // the sysfs entry exists, the /dev node does not
    CHECK(!d->capture_photo("../video0") && d->capture_photo("../video0").err.code == EINVAL);
    fs::remove_all(root);
}
// The live backend: whatever the machine has (WSL and CI have no IIO, cameras or GeoClue), every call returns a
// value or a clean error; nothing is changed on the machine.
TEST(linux_live_services) {
    auto d = m::device::linux_with("/sys", "/dev/input"); CHECK(d); if (!d) return;
    auto loc = d->open_sensor(m::sensor_kind::location, 1);
    std::printf("  location: %s\n", loc ? "GeoClue2 stream" : loc.err.what.c_str());
    auto b = d->brightness(); if (b) CHECK(*b >= 0 && *b <= 1); else CHECK(is_unsupported(b.err));
    auto k = d->keep_awake(true); std::printf("  keep_awake: %s\n", k ? "logind inhibitor held" : k.err.what.c_str());
    CHECK(d->keep_awake(false));
    auto cams = d->cameras(); CHECK(cams);
    if (cams && cams->empty()) CHECK(is_unsupported(d->capture_photo().err));
}
#endif

#if defined(__FreeBSD__)
// backlight(9) is read only; keep_awake is exercised only without X (with DISPLAY set it would change xset state).
TEST(freebsd_live_services) {
    auto d = m::device::open(); CHECK(d); if (!d || d->backend_name() != "freebsd") return;
    auto b = d->brightness(); std::printf("  brightness: %s\n", b ? std::to_string(*b).c_str() : b.err.what.c_str());
    if (b) CHECK(*b >= 0 && *b <= 1);
    if (!std::getenv("DISPLAY")) { CHECK(is_unsupported(d->keep_awake(true).err)); CHECK(d->keep_awake(false)); }
}
#endif

#if defined(_WIN32)
// Native Windows services on whatever the machine has. Read-only: brightness is read, never set; audio playback is
// 50 ms of silence; one notification only with FORGE_UI_TESTS=1; cameras are only opened with FORGE_UI_TESTS too.
TEST(windows_native_services) {
    auto d = m::device::open(); CHECK(d); if (!d || d->backend_name() != "windows") return;
    auto ok_or = [](const char* what, const forge::os::error& e) { std::printf("  %s: %s\n", what, e ? e.what.c_str() : "ok"); };
    auto ss = d->sensors(); CHECK(ss && !ss->empty() && ss->at(0).kind == m::sensor_kind::battery);
    if (ss) for (auto& s : *ss) std::printf("  sensor %s (%s)\n", m::sensor_name(s.kind), s.name.c_str());
    for (auto k : {m::sensor_kind::accelerometer, m::sensor_kind::light, m::sensor_kind::rotation_vector}) {
        bool listed = ss && std::any_of(ss->begin(), ss->end(), [&](auto& s) { return s.kind == k; });
        auto s = d->open_sensor(k, 20);
        if (!listed) CHECK(!s && is_unsupported(s.err));
        else if (s) { auto v = s->poll(1000, 1); std::printf("  %s: %zu event(s)\n", m::sensor_name(k), v.size()); }
    }
    auto cams = d->cameras(); ok_or("cameras", cams.err); if (cams) for (auto& c : *cams) std::printf("  camera %s\n", c.name.c_str());
    if (cams && cams->empty()) CHECK(is_unsupported(d->capture_photo().err));
    const char* ui = std::getenv("FORGE_UI_TESTS"); bool ui_on = ui && std::string(ui) == "1";
    if (cams && !cams->empty() && ui_on) {
        auto ph = d->capture_photo(); ok_or("capture_photo", ph.err);
        if (ph) CHECK(ph->size() > 4 && (*ph)[0] == 0xFF && (*ph)[1] == 0xD8);
    }
    auto b = d->brightness(); ok_or("brightness", b.err); if (b) { CHECK(*b >= 0 && *b <= 1); std::printf("  brightness %.2f\n", *b); }
    auto bt = d->bluetooth_scan(1000); ok_or("bluetooth_scan", bt.err);
    auto rec = d->record_audio(100, 16000); ok_or("record_audio", rec.err);
    if (rec) CHECK(rec->sample_rate == 16000 && !rec->samples.empty());
    m::audio_clip silence; silence.sample_rate = 16000; silence.samples.assign(800, 0);
    auto pl = d->play_audio(silence); ok_or("play_audio (silence)", pl.err);
    if (ui_on) {
        auto n = d->notify("forge test", "test_mobile windows_native_services"); ok_or("notify", n.err); CHECK(n);
        std::this_thread::sleep_for(std::chrono::seconds(3));   // the tray icon (and its balloon) go with the device
    }
}
#endif
