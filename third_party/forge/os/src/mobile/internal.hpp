#pragma once
// Internals shared by the forge_mobile backends.
#include <forge/os/mobile.hpp>
#include <forge/core/memory.hpp>
#include <atomic>
#include <condition_variable>
#include <deque>
#include <memory_resource>
#include <mutex>
#include <thread>

namespace forge::os::mobile::detail {

struct item { bool touch = false; sensor_event s; touch_event t; };

// Bounded MPSC queue. Storage comes from forge::mem (host budget, tag "mobile"); when the budget or
// the capacity is exhausted the oldest event is dropped and counted.
std::pmr::memory_resource* mobile_resource();
struct queue {
    explicit queue(std::size_t cap = 4096) : cap(cap), q(mobile_resource()) {}
    std::mutex m; std::condition_variable cv; std::size_t cap; std::pmr::deque<item> q;
    std::atomic<std::uint64_t> dropped{0}; std::atomic<bool> finished{false};
    void push(const item& it) {
        { std::lock_guard g(m);
          try { if (q.size() >= cap) { q.pop_front(); ++dropped; } q.push_back(it); }
          catch (...) { ++dropped; } }
        cv.notify_all();
    }
    void finish() { finished = true; cv.notify_all(); }
    template <class F> void drain(int timeout_ms, std::size_t max, F&& f) {
        std::unique_lock g(m);
        auto ready = [&] { return !q.empty() || finished.load(); };
        if (timeout_ms < 0) cv.wait(g, ready);
        else if (timeout_ms > 0) cv.wait_for(g, std::chrono::milliseconds(timeout_ms), ready);
        while (!q.empty() && max--) { f(q.front()); q.pop_front(); }
    }
};

// Process-wide routing for feed_touch/feed_sensor and report_* (app glue -> open streams).
struct bridge {
    static bridge& get();
    std::mutex m;
    std::vector<std::weak_ptr<queue>> touch;
    std::vector<std::pair<sensor_kind, std::weak_ptr<queue>>> sensor;
    std::optional<display_info> disp;
    std::array<std::optional<permission_state>, 5> perms{};
    std::array<bool, sensor_kind_count> provided{};   // provide_sensor: app glue feeds this kind
    action_hooks hooks;
    service_hook svc;
    std::vector<std::weak_ptr<std::function<void(app_event)>>> life;
    void add_touch(const std::shared_ptr<queue>& q) { std::lock_guard g(m); touch.push_back(q); }
    void add_sensor(sensor_kind k, const std::shared_ptr<queue>& q) { std::lock_guard g(m); sensor.emplace_back(k, q); }
};

struct backend {
    virtual ~backend() = default;
    virtual std::string name() const = 0;
    virtual result<std::vector<sensor_info>> sensors() { return {{}, unsupported("mobile sensors")}; }
    virtual result<sensor_stream> open_sensor(sensor_kind k, double) { return {{}, unsupported(std::string("sensor ") + sensor_name(k))}; }
    virtual result<touch_stream> open_touch() { return {{}, unsupported("touch input")}; }
    virtual status vibrate(int ms, double amp);
    virtual result<display_info> display();
    virtual result<orientation> screen_orientation() { auto d = display(); if (!d) return {{}, d.err}; return {d.value.orient, {}}; }
    virtual result<std::vector<camera_info>> cameras() { return {{}, unsupported("camera enumeration")}; }
    virtual result<permission_state> permission_status(permission p);
    virtual status request_permission(permission p);
    virtual status notify(std::string_view title, std::string_view body);
    virtual std::vector<sim_record> sim_log() const { return {}; }

    // Hook/sim answer for a service op (JSON), or nullopt to use the native implementation below.
    virtual std::optional<std::string> svc(std::string_view op, std::string_view args);
    // Native implementations. Defaults are the portable ones (services.cpp); platforms override.
    virtual result<power_info> power();
    virtual result<connectivity_info> connectivity();
    virtual result<std::vector<ble_device>> bluetooth_scan(int) { return {{}, unsupported("bluetooth scan")}; }
    virtual result<bool> nfc_available() { return {{}, unsupported("NFC")}; }
    virtual result<std::string> clipboard_get();
    virtual status clipboard_set(std::string_view t);
    virtual result<double> brightness() { return {{}, unsupported("screen brightness")}; }
    virtual status set_brightness(double) { return {{}, unsupported("screen brightness")}; }
    virtual status keep_awake(bool) { return {{}, unsupported("keep awake")}; }
    virtual status lock_orientation(orientation) { return {{}, unsupported("orientation lock")}; }
    virtual status show_keyboard(bool) { return {{}, unsupported("soft keyboard")}; }
    virtual result<biometric_info> biometrics() { return {{}, unsupported("biometrics")}; }
    virtual result<device_info> info();
    virtual result<storage_paths> paths();
    virtual status open_url(std::string_view url);
    virtual status share(std::string_view) { return {{}, unsupported("share sheet")}; }
    virtual status torch(bool) { return {{}, unsupported("torch")}; }
    virtual result<std::vector<std::uint8_t>> capture_photo(std::string_view) { return {{}, unsupported("camera capture")}; }
    virtual result<audio_clip> record_audio(int, int) { return {{}, unsupported("audio recording")}; }
    virtual status play_audio(const audio_clip&) { return {{}, unsupported("audio playback")}; }
    virtual result<gnss_status> gnss() { return {{}, unsupported("GNSS status")}; }
    virtual void lifecycle_started();   // on_lifecycle subscribed: default maps os::memory_pressure critical -> low_memory; sim replays its trace

    // Stream construction (backends are friends of the stream types through this base).
    static sensor_stream make_sensor(sensor_kind k, std::shared_ptr<queue> q, std::shared_ptr<void> keep) {
        sensor_stream s; s.kind_ = k; s.q_ = std::move(q); s.keep_ = std::move(keep); return s;
    }
    static touch_stream make_touch(std::shared_ptr<queue> q, std::shared_ptr<void> keep) {
        touch_stream s; s.q_ = std::move(q); s.keep_ = std::move(keep); return s;
    }
};

std::shared_ptr<backend> make_null();
// open_url policy: absolute URL, RFC 3986 scheme on the set_url_schemes allow-list, no controls or spaces.
status check_url(std::string_view url);
void dispatch_lifecycle(app_event e, bool drive_mem = true);
// Run argv[0] from PATH and wait (unix: posix_spawnp; Windows: CreateProcess). Returns exit code or -1 if not found;
// with `out`, stdout is captured.
int run_tool(const std::vector<std::string>& argv, std::string* out = nullptr, int timeout_ms = 15000);
#if defined(_WIN32)
std::shared_ptr<backend> make_windows();
#endif
#if defined(__FreeBSD__)
std::shared_ptr<backend> make_freebsd();
#endif
std::shared_ptr<backend> make_sim(std::string_view json, bool realtime, error& err);
#if defined(__linux__) && !defined(__ANDROID__)
std::shared_ptr<backend> make_linux(std::string sysfs, std::string input);
#endif
#if defined(__ANDROID__)
std::shared_ptr<backend> make_android();
#endif
#if defined(__APPLE__)
std::shared_ptr<backend> make_apple();   // iOS/iPadOS: CoreMotion etc.; macOS: returns null backend
#endif
// Background thread owner: stop flag + interruptible sleep; joins on destruction (RAII, no detached threads).
struct worker {
    std::atomic<bool> stop{false}; std::mutex m; std::condition_variable cv; std::thread t;
    template <class F> void start(F&& f) { t = std::thread(std::forward<F>(f)); }
    bool sleep_ns(std::uint64_t ns) {   // false when asked to stop
        std::unique_lock g(m); return !cv.wait_for(g, std::chrono::nanoseconds(ns), [&] { return stop.load(); });
    }
    ~worker() { { std::lock_guard g(m); stop = true; } cv.notify_all(); if (t.joinable()) t.join(); }
};
inline std::uint64_t now_ns() { return monotonic_ns(); }

// ---- pure helpers shared by the backends (mobile.cpp; unit-tested in tests/test_mobile.cpp) ---------------
// Interleaved float PCM: channel mapping (average to mono, duplicate mono, else copy the first channels and
// zero-fill) and linear-interpolation resampling.
std::vector<float> remix(const float* in, std::size_t frames, int in_ch, int in_rate, int out_ch, int out_rate);
// Motion-JPEG frames (UVC, V4L2 MJPEG) usually omit the Huffman tables; this inserts the standard ones (ITU T.81
// K.3) before the first SOS when the frame has no DHT. Anything that is not a parseable JPEG comes back unchanged.
std::vector<std::uint8_t> jpeg_with_dht(std::vector<std::uint8_t> frame);
// IIO channels by attribute name: in_<type>[<index>][_<modifier>]_{raw,input} (accel/anglvel/magn/gravity need
// x, y, z modifiers; temp prefers the "ambient" modifier; rot_quaternion is one 4-value attribute). One channel
// per kind, lowest index first. `prefix` is the attribute stem ("in_accel", "in_illuminance0", "in_temp_ambient"),
// `type_prefix` the shared-attribute stem ("in_accel"); value = (raw + offset) * scale * mul.
struct iio_channel { sensor_kind kind{}; std::string prefix, type_prefix; int axes = 1; double mul = 1; };
std::vector<iio_channel> iio_channels(std::vector<std::string> attrs);
// `xset q` (X11 screen saver and DPMS state), for restoring what keep_awake changed. -1: not reported.
struct xset_saver { int timeout = -1, cycle = -1, dpms = -1; };
xset_saver parse_xset_q(std::string_view text);

} // namespace forge::os::mobile::detail
