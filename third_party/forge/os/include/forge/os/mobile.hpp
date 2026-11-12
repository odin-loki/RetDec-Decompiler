#pragma once
// forge::os::mobile — sensors, touch, haptics, display, cameras, permissions and notifications.
// Design: docs/mobile.md.  Built as the forge_mobile library (os/src/mobile/).
//   Android   NDK: ASensorManager + ALooper event queues, AInputEvent motion events fed by the app's
//             NativeActivity/GameActivity glue (android_feed_motion), ACameraManager, JNI hooks for the rest.
//   iOS       CoreMotion, CoreLocation, UIKit (UIScreen, UIDevice, UIImpactFeedbackGenerator,
//             FORGETouchView), AVFoundation, UserNotifications (apple.mm, Apple targets only).
//   macOS     IOKit power assertions, NSWorkspace, UserNotifications (apple.mm); touch/location from app glue.
//   Windows   WM_POINTER via windows_feed_pointer, display/DPI, power requests, Known Folders, ShellExecuteW.
//   Linux     IIO sensors (/sys/bus/iio), evdev multitouch (/dev/input) + force feedback, DRM/fbdev
//             display, V4L2 cameras, power_supply battery, notify-send.
//   FreeBSD   sysctl thermal + ACPI battery, evdev multitouch.
//   other     null backend: every call returns os::unsupported(...).
//   sim       a JSON trace replayed on any platform (device::simulated / FORGE_MOBILE_TRACE).
// All event queues are bounded and charge forge::mem's host budget (tag "mobile"); a full queue drops
// the oldest event and counts it in dropped().
#include <forge/os/os.hpp>
#include <forge/os/power.hpp>
#include <functional>
#include <array>
#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace forge::os::mobile {

enum class sensor_kind { accelerometer, gyroscope, magnetometer, light, proximity, pressure, location, battery,
                         step_counter, step_detector, rotation_vector, gravity, linear_acceleration, heart_rate, temperature, humidity };
inline constexpr int sensor_kind_count = 16;
const char* sensor_name(sensor_kind k);
std::optional<sensor_kind> parse_sensor(std::string_view s);

struct sensor_info { sensor_kind kind{}; std::string name, vendor; double max_rate_hz = 0, range = 0, resolution = 0; };

// v[] layout: accelerometer m/s^2 xyz; gyroscope rad/s xyz; magnetometer uT xyz; light lux; proximity cm;
// pressure hPa; location lat, lon, altitude m, accuracy m; battery percent, charging (0/1);
// step_counter steps since boot; step_detector 1 per step; rotation_vector quaternion x,y,z,w; gravity and
// linear_acceleration m/s^2 xyz; heart_rate bpm; temperature degC; humidity %RH.
struct sensor_event { sensor_kind kind{}; std::uint64_t t_ns = 0; std::array<double, 4> v{}; int n = 0; };

enum class touch_phase { down, move, up, cancel };
const char* touch_phase_name(touch_phase p);
struct touch_event { std::uint64_t t_ns = 0; touch_phase phase{}; int id = 0; double x = 0, y = 0, pressure = 1; int pointers = 1; };

enum class gesture_kind { tap, double_tap, long_press, swipe, pinch, rotate };
const char* gesture_name(gesture_kind g);
struct gesture { gesture_kind kind{}; std::uint64_t t_ns = 0; double x = 0, y = 0, dx = 0, dy = 0, scale = 1, rotation = 0; };   // rotation radians (rotate)

// Pure state machine over touch events; used by touch_stream::gestures and testable on its own.
struct gesture_config { double tap_slop_px = 24, swipe_min_px = 80; std::uint64_t tap_max_ns = 300'000'000, double_tap_ns = 350'000'000, long_press_ns = 500'000'000; };
class gesture_recognizer {
public:
    explicit gesture_recognizer(gesture_config c = {}) : c_(c) {}
    std::vector<gesture> feed(const touch_event& e);
private:
    struct pt { int id; double x0, y0, x, y; std::uint64_t t0; bool long_fired; };
    gesture_config c_; std::vector<pt> live_; double pinch0_ = 0, pinch_last_ = 1, angle0_ = 0, angle_last_ = 0; bool multi_ = false;
    std::uint64_t last_tap_t_ = 0; double last_tap_x_ = 0, last_tap_y_ = 0; bool have_tap_ = false;
};

enum class orientation { unknown, portrait, portrait_upside_down, landscape_left, landscape_right };
const char* orientation_name(orientation o);
std::optional<orientation> parse_orientation(std::string_view s);
struct display_info { int width_px = 0, height_px = 0; double dpi = 0, scale = 1, refresh_hz = 0; orientation orient = orientation::unknown; };

struct camera_info { std::string id, name; bool front = false; };

enum class permission { sensors, location, camera, notifications, vibrate };
const char* permission_name(permission p);
std::optional<permission> parse_permission(std::string_view s);
enum class permission_state { granted, denied, not_determined, not_required };
const char* permission_state_name(permission_state s);

namespace detail { struct queue; struct backend; }

// A running sensor subscription. Stops the hardware sensor when destroyed.
class sensor_stream {
public:
    sensor_stream() = default;
    sensor_stream(sensor_stream&&) noexcept; sensor_stream& operator=(sensor_stream&&) noexcept; ~sensor_stream();
    sensor_stream(const sensor_stream&) = delete; sensor_stream& operator=(const sensor_stream&) = delete;
    // Waits up to timeout_ms (0 = no wait, <0 = forever) for at least one event, then drains up to `max`.
    std::vector<sensor_event> poll(int timeout_ms = 0, std::size_t max = 1024);
    sensor_kind kind() const { return kind_; }
    std::uint64_t dropped() const;
    bool finished() const;    // the source ended (trace exhausted, device unplugged)
    explicit operator bool() const { return q_ != nullptr; }
private:
    friend struct detail::backend;
    std::shared_ptr<detail::queue> q_; std::shared_ptr<void> keep_; sensor_kind kind_{};
};

class touch_stream {
public:
    touch_stream() = default;
    touch_stream(touch_stream&&) noexcept; touch_stream& operator=(touch_stream&&) noexcept; ~touch_stream();
    touch_stream(const touch_stream&) = delete; touch_stream& operator=(const touch_stream&) = delete;
    std::vector<touch_event> poll(int timeout_ms = 0, std::size_t max = 1024);
    // Same events run through a gesture_recognizer (touches consumed here are not returned by poll()).
    std::vector<gesture> gestures(int timeout_ms = 0);
    std::uint64_t dropped() const;
    bool finished() const;
    explicit operator bool() const { return q_ != nullptr; }
private:
    friend struct detail::backend;
    std::shared_ptr<detail::queue> q_; std::shared_ptr<void> keep_; gesture_recognizer rec_;
};

struct sim_record { std::string what, detail; };

// ---- device services (every OS; not_supported where the platform has no such thing) -------------------
using power_info = os::power_state;
struct connectivity_info { bool online = false; std::string transport;   // wifi, cellular, ethernet, bluetooth, vpn, unknown
                           std::string ssid; int signal_percent = -1; bool metered = false; std::vector<std::string> interfaces; };
struct ble_device { std::string address, name; int rssi = 0; };
struct biometric_info { bool available = false, enrolled = false; std::string kind; };   // fingerprint, face, iris
enum class form_factor { unknown, phone, tablet, watch, tv, desktop, laptop, car };
const char* form_factor_name(form_factor f);
struct device_info { std::string manufacturer, model, os_name, os_version, arch; form_factor form = form_factor::unknown; };
struct storage_paths { std::string home, documents, downloads, cache, data, temp; };
struct gnss_status { bool fix = false; int satellites_visible = -1, satellites_used = -1; };
struct audio_clip { int sample_rate = 16000, channels = 1; std::vector<std::int16_t> samples; };
enum class app_event { foreground, background, low_memory, terminate };
const char* app_event_name(app_event e);
std::optional<app_event> parse_app_event(std::string_view s);

// Software geofencing over location events (works on every backend that produces location).
struct geofence { std::string id; double lat = 0, lon = 0, radius_m = 100; };
struct geofence_event { std::string id; bool entered = false; std::uint64_t t_ns = 0; };
class geofence_monitor {
public:
    void add(geofence g);
    bool remove(std::string_view id);
    std::vector<geofence_event> feed(const sensor_event& location);
    static double distance_m(double lat1, double lon1, double lat2, double lon2);   // haversine
private:
    struct st { geofence g; int inside = -1; }; std::vector<st> f_;
};

// RAII lifecycle subscription; the callback runs on the thread that delivers the event.
class subscription {
public:
    subscription() = default; explicit subscription(std::shared_ptr<void> k) : k_(std::move(k)) {}
    void reset() { k_.reset(); } explicit operator bool() const { return k_ != nullptr; }
private: std::shared_ptr<void> k_;
};   // vibrate/notify/permission requests seen by the sim backend

// The device. Move-only RAII owner of one backend.
class device {
public:
    // Platform backend; FORGE_MOBILE_TRACE=<file.json> selects the simulator instead.
    static result<device> open();
    static result<device> null_device();
    static result<device> simulated(std::string_view json_trace, bool realtime = false);
    static result<device> simulated_file(const std::string& path, bool realtime = false);
#if defined(__linux__)
    // Linux backend against an explicit sysfs root and input directory (tests use fake trees).
    static result<device> linux_with(std::string sysfs_root, std::string input_dir);
#endif
    device() = default;
    device(device&&) noexcept; device& operator=(device&&) noexcept; ~device();
    device(const device&) = delete; device& operator=(const device&) = delete;
    explicit operator bool() const { return b_ != nullptr; }

    std::string backend_name() const;
    result<std::vector<sensor_info>> sensors();
    bool has_sensor(sensor_kind k);
    result<sensor_stream> open_sensor(sensor_kind k, double rate_hz = 50);
    result<touch_stream> open_touch();
    status vibrate(int ms, double amplitude = 1.0);
    result<display_info> display();
    result<orientation> screen_orientation();
    result<std::vector<camera_info>> cameras();
    result<permission_state> permission_status(permission p);
    status request_permission(permission p);
    status notify(std::string_view title, std::string_view body);
    std::vector<sim_record> sim_log() const;   // empty unless simulated

    result<power_info> power();
    result<connectivity_info> connectivity();
    result<std::vector<ble_device>> bluetooth_scan(int timeout_ms = 4000);
    result<bool> nfc_available();
    result<std::string> clipboard_get();
    status clipboard_set(std::string_view text);
    result<double> brightness();                 // 0..1
    status set_brightness(double level);
    status keep_awake(bool on);
    status lock_orientation(orientation o);       // orientation::unknown unlocks
    status show_keyboard(bool show);
    result<biometric_info> biometrics();
    result<device_info> info();
    result<storage_paths> paths();
    status open_url(std::string_view url);
    status share(std::string_view text);
    status torch(bool on);
    result<std::vector<std::uint8_t>> capture_photo(std::string_view camera_id = {});   // JPEG bytes
    result<audio_clip> record_audio(int ms, int sample_rate = 16000);
    status play_audio(const audio_clip& clip);
    status haptic_pattern(const std::vector<int>& on_off_ms, double amplitude = 1.0);
    result<gnss_status> gnss();
    // Foreground/background/low_memory/terminate. low_memory also drives forge::mem (critical pressure:
    // every cache is dropped) whether or not anyone subscribed.
    subscription on_lifecycle(std::function<void(app_event)> cb);
private:
    std::shared_ptr<detail::backend> b_;
};

// ---- glue entry points for app code (Android NativeActivity/GameActivity, iOS views, JNI) -------------
// Events pushed here reach every open touch_stream / sensor_stream of the platform device.
void feed_touch(const touch_event& e);
void feed_sensor(const sensor_event& e);
// Declares that app glue feeds sensor kind k through feed_sensor (WinRT sensors, CoreLocation on macOS, GeoClue on
// Linux, ...). open_sensor(k) then returns a bridge-only stream wherever the backend has no native source for k,
// and sensors() lists k. Pass false to withdraw.
void provide_sensor(sensor_kind k, bool provided = true);
// Values only app code can know (Activity/Context/JNI): they override probing where the backend can't ask.
void report_display(const display_info& d);
void report_permission(permission p, permission_state s);
// Called by app code to perform actions that need a Java/ObjC context (vibrate, notify, request permission).
struct action_hooks {
    std::function<bool(int ms, double amp)> vibrate;
    std::function<bool(std::string_view title, std::string_view body)> notify;
    std::function<bool(permission)> request_permission;
};
void set_action_hooks(action_hooks h);
// One string-keyed hook covering every device service, for app glue in Kotlin/Java (JNI), Swift/ObjC or C#.
// op is the device method name ("connectivity", "torch", ...), args a JSON object; return the JSON result
// (same shape as the sim trace "services" entries) or std::nullopt to fall through to the native backend.
using service_hook = std::function<std::optional<std::string>(std::string_view op, std::string_view args_json)>;
void set_service_hook(service_hook h);
void feed_lifecycle(app_event e);
// URL schemes open_url accepts (case-insensitive). Default: http, https, mailto, tel. Anything else (file:, a bare
// path, javascript:, custom handlers, a leading '-') is refused before reaching the hook or the OS opener.
void set_url_schemes(std::vector<std::string> schemes);
#if defined(__ANDROID__)
// Pass the AInputEvent* from AInputQueue_getEvent (NativeActivity) or the GameActivity motion event
// translated by the app; returns true when it was a motion event and was consumed.
bool android_feed_motion(const void* ainput_event);
#endif
#if defined(_WIN32)
// Call from the app's WndProc with WM_POINTERDOWN/UPDATE/UP (and WM_POINTERCAPTURECHANGED); returns true when consumed.
bool windows_feed_pointer(void* hwnd, unsigned msg, std::uintptr_t wparam, std::intptr_t lparam);
#endif

} // namespace forge::os::mobile
