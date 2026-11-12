// forge_mobile Linux backend (postmarketOS, Mobian, PinePhone, Librem 5, any IIO/evdev device):
//   sensors   IIO sysfs channels under <sysfs>/bus/iio/devices found by attribute name (iio_channels: accel,
//             anglvel, magn, gravity, illuminance, proximity, pressure, temp, humidityrelative, steps,
//             rot_quaternion; indexed and modified names), sampled at the requested rate; battery from
//             <sysfs>/class/power_supply. Location: GeoClue2 over the system D-Bus (sd-bus from libsystemd.so.0,
//             loaded at run time), live /sys only.
//   touch     evdev nodes in <input> whose <sysfs>/class/input/<node>/device/capabilities/abs has
//             ABS_MT_POSITION_X (multitouch protocol B), or ABS_X + BTN_TOUCH (single touch).
//   haptics   evdev force feedback (FF_RUMBLE) — the vibra motor on most phones.
//   display   DRM connector modes, else fbdev virtual_size; orientation from the accelerometer.
//             Brightness: backlight sysfs, else logind Session.SetBrightness (no root needed). keep_awake: logind
//             Inhibit("idle:sleep", block), the fd held until keep_awake(false).
//   cameras   <sysfs>/class/video4linux; capture_photo: one MJPEG/JPEG frame through V4L2 mmap streaming.
//   notify: notify-send.  permissions: not_required.
#include "internal.hpp"
#if defined(__linux__) && !defined(__ANDROID__)
#include "evdev.hpp"
#include <cerrno>
#include <cmath>
#include <cstring>
#include <fcntl.h>
#include <linux/input.h>
#include <poll.h>
#include <spawn.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#include <fstream>
#include <linux/videodev2.h>
#include <dlfcn.h>
#include <sys/mman.h>
#include <type_traits>
#include <map>
#include <forge/os/process.hpp>

extern char** environ;

namespace forge::os::mobile::detail {
namespace {
std::string slurp(const std::string& p) { std::ifstream f(p); if (!f) return {}; std::string s((std::istreambuf_iterator<char>(f)), {}); while (!s.empty() && (s.back() == '\n' || s.back() == ' ')) s.pop_back(); return s; }
bool exists(const std::string& p) { struct stat st; return ::stat(p.c_str(), &st) == 0; }
std::vector<std::string> ls(const std::string& d) { std::vector<std::string> v; if (auto l = list_dir(d)) for (auto& e : l.value) v.push_back(e.name); std::sort(v.begin(), v.end()); return v; }
// sysfs attribute write; the value only lands when write() succeeds (an ofstream reports failure at flush, if ever).
status write_attr(const std::string& p, const std::string& v) {
    int fd = ::open(p.c_str(), O_WRONLY | O_CLOEXEC); if (fd < 0) return {{}, last_error("open " + p)};
    ssize_t n = ::write(fd, v.data(), v.size()); auto e = n == ssize_t(v.size()) ? error{} : last_error("write " + p);
    ::close(fd); return {{}, e};
}
// nmcli -t -e yes: fields separated by ':', with ':' and '\' inside values escaped by a backslash.
std::vector<std::string> nm_fields(const std::string& line) {
    std::vector<std::string> f(1);
    for (std::size_t i = 0; i < line.size(); ++i) {
        if (line[i] == '\\' && i + 1 < line.size()) f.back() += line[++i];
        else if (line[i] == ':') f.emplace_back();
        else f.back() += line[i];
    }
    return f;
}
bool num(const std::string& p, double& out) { auto s = slurp(p); if (s.empty()) return false; char* e = nullptr; out = std::strtod(s.c_str(), &e); return e != s.c_str(); }

// Bit test on a sysfs capability mask ("hex words, most significant first").
bool cap_bit(const std::string& mask, int bit) {
    std::vector<unsigned long long> w; std::size_t i = 0;
    while (i < mask.size()) { while (i < mask.size() && mask[i] == ' ') ++i; std::size_t j = i; while (j < mask.size() && mask[j] != ' ') ++j; if (j > i) w.push_back(std::stoull(mask.substr(i, j - i), nullptr, 16)); i = j; }
    std::size_t word = std::size_t(bit / 64); if (word >= w.size()) return false;
    return (w[w.size() - 1 - word] >> (bit % 64)) & 1ull;
}

struct channel { std::string dev; iio_channel c; };   // value = (raw+offset)*scale*mul
struct iio_sensor { sensor_info info; std::vector<channel> ch; };

// Shared attributes fall back from the channel (in_accel_x_scale) to the stem (in_accel_scale, in_illuminance0_scale)
// to the type (in_illuminance_scale).
bool chan_attr(const channel& c, const std::string& axis, const char* what, double& out) {
    return num(c.dev + "/" + c.c.prefix + axis + what, out) || num(c.dev + "/" + c.c.prefix + what, out) || num(c.dev + "/" + c.c.type_prefix + what, out);
}
bool read_chan(const channel& c, sensor_event& e) {
    if (c.c.axes == 4) {   // in_rot_quaternion_raw: "x y z w" in one attribute (HID order)
        auto s = slurp(c.dev + "/" + c.c.prefix + "_raw"); if (s.empty()) return false;
        double sc = 1; chan_attr(c, "", "_scale", sc);
        const char* p = s.c_str();
        for (int i = 0; i < 4; ++i) { char* end = nullptr; double v = std::strtod(p, &end); if (end == p) return false; e.v[std::size_t(i)] = v * sc * c.c.mul; p = end; }
        e.n = 4; return true;
    }
    static const char* ax[] = {"_x", "_y", "_z"};
    for (int i = 0; i < c.c.axes; ++i) {
        std::string a = c.c.axes > 1 ? ax[i] : "";
        double v;
        if (num(c.dev + "/" + c.c.prefix + a + "_input", v)) { e.v[std::size_t(i)] = v * c.c.mul; continue; }
        double raw; if (!num(c.dev + "/" + c.c.prefix + a + "_raw", raw)) return false;
        double off = 0, sc = 1; chan_attr(c, a, "_offset", off); chan_attr(c, a, "_scale", sc);
        e.v[std::size_t(i)] = (raw + off) * sc * c.c.mul;
    }
    e.n = c.c.axes; return true;
}

// ---- sd-bus (libsystemd.so.0, loaded at run time; same approach as background.hpp's logind inhibitor)
struct bus_error { const char* name; const char* message; int need_free; };
struct sdbus {
    int (*open_system)(void**) = nullptr;
    int (*call)(void*, const char*, const char*, const char*, const char*, bus_error*, void**, const char*, ...) = nullptr;
    int (*read)(void*, const char*, ...) = nullptr;
    int (*get_prop)(void*, const char*, const char*, const char*, const char*, bus_error*, void**, const char*) = nullptr;
    int (*set_prop)(void*, const char*, const char*, const char*, const char*, bus_error*, const char*, ...) = nullptr;
    void* (*msg_unref)(void*) = nullptr; void* (*bus_unref)(void*) = nullptr; void (*err_free)(bus_error*) = nullptr;
    bool ok() const { return open_system && call && read && get_prop && set_prop && msg_unref && bus_unref && err_free; }
    static const sdbus& get() {
        static const sdbus a = [] {
            sdbus x; void* h = ::dlopen("libsystemd.so.0", RTLD_LAZY | RTLD_LOCAL); if (!h) return x;
            auto f = [&](auto& p, const char* n) { p = reinterpret_cast<std::remove_reference_t<decltype(p)>>(::dlsym(h, n)); };
            f(x.open_system, "sd_bus_open_system"); f(x.call, "sd_bus_call_method"); f(x.read, "sd_bus_message_read");
            f(x.get_prop, "sd_bus_get_property"); f(x.set_prop, "sd_bus_set_property"); f(x.msg_unref, "sd_bus_message_unref");
            f(x.bus_unref, "sd_bus_flush_close_unref"); f(x.err_free, "sd_bus_error_free");
            return x; }();
        return a;
    }
};
// Owning system-bus connection; empty when there is no libsystemd or no system bus.
struct bus_conn {
    void* b = nullptr;
    bus_conn() { auto& a = sdbus::get(); if (a.ok() && a.open_system(&b) < 0) b = nullptr; }
    ~bus_conn() { if (b) sdbus::get().bus_unref(b); }
    bus_conn(const bus_conn&) = delete; bus_conn& operator=(const bus_conn&) = delete;
    explicit operator bool() const { return b != nullptr; }
};
// Error from a failed call: unsupported when the service is absent, else EACCES/EIO with the D-Bus message.
error bus_fail(int r, bus_error& e, const std::string& what) {
    std::string n = e.name ? e.name : "", m = e.message ? e.message : (n.empty() ? error_text(-r) : n);
    sdbus::get().err_free(&e);
    if (n == "org.freedesktop.DBus.Error.ServiceUnknown" || n == "org.freedesktop.DBus.Error.NameHasNoOwner" || n == "org.freedesktop.DBus.Error.UnknownMethod"
        || n == "org.freedesktop.DBus.Error.UnknownObject" || n == "org.freedesktop.DBus.Error.UnknownInterface")
        return unsupported(what + " (" + m + ")");
    bool denied = n == "org.freedesktop.DBus.Error.AccessDenied" || n == "org.freedesktop.DBus.Error.AuthFailed" || n.find("NotAuthorized") != std::string::npos || r == -EACCES || r == -EPERM;
    return {denied ? EACCES : (r < 0 ? -r : EIO), what + ": " + m, err_domain::posix};
}
double bus_get_double(void* bus, const char* path, const char* iface, const char* prop, bool& ok) {
    auto& a = sdbus::get(); bus_error e{}; void* reply = nullptr; double v = 0;
    ok = a.get_prop(bus, "org.freedesktop.GeoClue2", path, iface, prop, &e, &reply, "d") >= 0 && reply && a.read(reply, "d", &v) >= 0;
    if (reply) a.msg_unref(reply); a.err_free(&e); return v;
}
std::string bus_get_path(void* bus, const char* path, const char* iface, const char* prop) {
    auto& a = sdbus::get(); bus_error e{}; void* reply = nullptr; const char* v = nullptr; std::string out;
    if (a.get_prop(bus, "org.freedesktop.GeoClue2", path, iface, prop, &e, &reply, "o") >= 0 && reply && a.read(reply, "o", &v) >= 0 && v) out = v;
    if (reply) a.msg_unref(reply); a.err_free(&e); return out;
}

// GeoClue2 client: Manager.GetClient, DesktopId + accuracy, Start; Location is polled (no signal match, so nothing
// queues up on the connection) and each new Location object is read once.
struct geoclue {
    bus_conn bus; std::string client; std::string last;
    status start() {
        auto& a = sdbus::get();
        if (!bus) return {{}, unsupported("location (no system D-Bus / libsystemd for GeoClue2)")};
        bus_error e{}; void* reply = nullptr;
        int r = a.call(bus.b, "org.freedesktop.GeoClue2", "/org/freedesktop/GeoClue2/Manager", "org.freedesktop.GeoClue2.Manager", "GetClient", &e, &reply, "");
        if (r < 0) { if (reply) a.msg_unref(reply); return {{}, bus_fail(r, e, "GeoClue2 GetClient")}; }
        const char* p = nullptr; if (a.read(reply, "o", &p) >= 0 && p) client = p; a.msg_unref(reply); a.err_free(&e);
        if (client.empty()) return {{}, {EIO, "GeoClue2 GetClient: no client path", err_domain::posix}};
        const char* ci = "org.freedesktop.GeoClue2.Client";
        e = {}; r = a.set_prop(bus.b, "org.freedesktop.GeoClue2", client.c_str(), ci, "DesktopId", &e, "s", "forge");
        if (r < 0) return {{}, bus_fail(r, e, "GeoClue2 DesktopId")};
        a.err_free(&e); e = {};
        a.set_prop(bus.b, "org.freedesktop.GeoClue2", client.c_str(), ci, "RequestedAccuracyLevel", &e, "u", std::uint32_t(8) /* EXACT */); a.err_free(&e);
        e = {}; reply = nullptr;
        r = a.call(bus.b, "org.freedesktop.GeoClue2", client.c_str(), ci, "Start", &e, &reply, "");
        if (reply) a.msg_unref(reply);
        if (r < 0) return {{}, bus_fail(r, e, "GeoClue2 Start (is the app allowed by the GeoClue agent?)")};
        a.err_free(&e); return {};
    }
    // A new fix since the last call, if any.
    bool poll(sensor_event& ev) {
        auto loc = bus_get_path(bus.b, client.c_str(), "org.freedesktop.GeoClue2.Client", "Location");
        if (loc.empty() || loc == "/" || loc == last) return false;
        const char* li = "org.freedesktop.GeoClue2.Location"; bool ok1, ok2, ok3, ok4;
        double lat = bus_get_double(bus.b, loc.c_str(), li, "Latitude", ok1), lon = bus_get_double(bus.b, loc.c_str(), li, "Longitude", ok2);
        double alt = bus_get_double(bus.b, loc.c_str(), li, "Altitude", ok3), acc = bus_get_double(bus.b, loc.c_str(), li, "Accuracy", ok4);
        if (!ok1 || !ok2) return false;
        last = loc; ev = {}; ev.kind = sensor_kind::location; ev.t_ns = now_ns();
        ev.v = {lat, lon, ok3 && alt > -1e300 ? alt : 0.0, ok4 ? acc : 0.0}; ev.n = 4; return true;   // GeoClue: -DBL_MAX altitude = unknown
    }
    ~geoclue() {
        if (!bus || client.empty()) return;
        auto& a = sdbus::get(); bus_error e{}; void* reply = nullptr;
        a.call(bus.b, "org.freedesktop.GeoClue2", client.c_str(), "org.freedesktop.GeoClue2.Client", "Stop", &e, &reply, "");
        if (reply) a.msg_unref(reply); a.err_free(&e);
    }
};

// ---- V4L2 single-frame capture (mmap streaming). Motion-JPEG / JPEG formats only: the frame is returned as delivered
// with the standard Huffman tables added when the camera leaves them out.
result<std::vector<std::uint8_t>> v4l2_capture_jpeg(const std::string& node) {
    int fd = ::open(node.c_str(), O_RDWR | O_CLOEXEC);
    if (fd < 0) return {{}, errno == ENOENT ? unsupported("camera capture (" + node + " missing)") : last_error("open " + node)};
    struct closer { int fd; std::vector<std::pair<void*, std::size_t>> maps; bool on = false;
        ~closer() { if (on) { int t = V4L2_BUF_TYPE_VIDEO_CAPTURE; ::ioctl(fd, VIDIOC_STREAMOFF, &t); } for (auto& m : maps) ::munmap(m.first, m.second); ::close(fd); } } cl{fd, {}};
    auto xioctl = [&](unsigned long req, void* arg) { int r; do r = ::ioctl(fd, req, arg); while (r < 0 && errno == EINTR); return r; };
    std::uint32_t fmt_jpeg = 0;
    for (std::uint32_t i = 0; i < 64; ++i) {
        v4l2_fmtdesc d{}; d.index = i; d.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        if (xioctl(VIDIOC_ENUM_FMT, &d) < 0) break;
        if (d.pixelformat == V4L2_PIX_FMT_MJPEG || d.pixelformat == V4L2_PIX_FMT_JPEG) { fmt_jpeg = d.pixelformat; break; }
    }
    if (!fmt_jpeg) return {{}, unsupported("camera capture (" + node + " offers no MJPEG/JPEG format; raw YUV is not encoded)")};
    v4l2_format f{}; f.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    if (xioctl(VIDIOC_G_FMT, &f) < 0) return {{}, last_error("VIDIOC_G_FMT")};
    // Largest discrete frame size of the JPEG format.
    for (std::uint32_t i = 0; i < 256; ++i) {
        v4l2_frmsizeenum s{}; s.index = i; s.pixel_format = fmt_jpeg;
        if (xioctl(VIDIOC_ENUM_FRAMESIZES, &s) < 0) break;
        if (s.type == V4L2_FRMSIZE_TYPE_DISCRETE) { if (std::uint64_t(s.discrete.width) * s.discrete.height > std::uint64_t(f.fmt.pix.width) * f.fmt.pix.height || i == 0) { f.fmt.pix.width = s.discrete.width; f.fmt.pix.height = s.discrete.height; } }
        else { f.fmt.pix.width = s.stepwise.max_width; f.fmt.pix.height = s.stepwise.max_height; break; }
    }
    f.fmt.pix.pixelformat = fmt_jpeg; f.fmt.pix.field = V4L2_FIELD_ANY;
    if (xioctl(VIDIOC_S_FMT, &f) < 0) return {{}, last_error("VIDIOC_S_FMT")};
    if (f.fmt.pix.pixelformat != fmt_jpeg) return {{}, unsupported("camera capture (driver refused MJPEG)")};
    v4l2_requestbuffers rb{}; rb.count = 4; rb.type = V4L2_BUF_TYPE_VIDEO_CAPTURE; rb.memory = V4L2_MEMORY_MMAP;
    if (xioctl(VIDIOC_REQBUFS, &rb) < 0) return {{}, last_error("VIDIOC_REQBUFS")};
    if (rb.count < 1) return {{}, {ENOMEM, "VIDIOC_REQBUFS: no buffers", err_domain::posix}};
    for (std::uint32_t i = 0; i < rb.count; ++i) {
        v4l2_buffer b{}; b.type = V4L2_BUF_TYPE_VIDEO_CAPTURE; b.memory = V4L2_MEMORY_MMAP; b.index = i;
        if (xioctl(VIDIOC_QUERYBUF, &b) < 0) return {{}, last_error("VIDIOC_QUERYBUF")};
        void* p = ::mmap(nullptr, b.length, PROT_READ | PROT_WRITE, MAP_SHARED, fd, b.m.offset);
        if (p == MAP_FAILED) return {{}, last_error("mmap video buffer")};
        cl.maps.emplace_back(p, b.length);
        if (xioctl(VIDIOC_QBUF, &b) < 0) return {{}, last_error("VIDIOC_QBUF")};
    }
    int type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    if (xioctl(VIDIOC_STREAMON, &type) < 0) return {{}, last_error("VIDIOC_STREAMON")};
    cl.on = true;
    // Skip a few frames so auto exposure settles; give up after 5 s without a frame.
    std::vector<std::uint8_t> frame;
    for (int got = 0; got < 4;) {
        pollfd p{fd, POLLIN, 0}; int pr = ::poll(&p, 1, 5000);
        if (pr < 0 && errno == EINTR) continue;
        if (pr <= 0) { if (!frame.empty()) break; return {{}, pr == 0 ? error{ETIMEDOUT, "camera capture: no frame within 5 s", err_domain::posix} : last_error("poll video")}; }
        v4l2_buffer b{}; b.type = V4L2_BUF_TYPE_VIDEO_CAPTURE; b.memory = V4L2_MEMORY_MMAP;
        if (xioctl(VIDIOC_DQBUF, &b) < 0) { if (errno == EAGAIN) continue; return {{}, last_error("VIDIOC_DQBUF")}; }
        if (b.index < cl.maps.size() && b.bytesused > 0 && !(b.flags & V4L2_BUF_FLAG_ERROR)) {
            auto* p8 = static_cast<const std::uint8_t*>(cl.maps[b.index].first);
            frame.assign(p8, p8 + std::min<std::size_t>(b.bytesused, cl.maps[b.index].second)); ++got;
        }
        if (xioctl(VIDIOC_QBUF, &b) < 0) break;
    }
    if (frame.empty()) return {{}, {EIO, "camera capture: no frame", err_domain::posix}};
    return {jpeg_with_dht(std::move(frame)), {}};
}

struct linux_backend final : backend, std::enable_shared_from_this<linux_backend> {
    std::string sys, input;
    bool real;   // the live /sys: D-Bus services (GeoClue2, logind) are only consulted then, never for fake test trees
    std::mutex awake_m; int inhibit_fd = -1;
    linux_backend(std::string s, std::string i) : sys(std::move(s)), input(std::move(i)) { real = sys == "/sys"; }
    ~linux_backend() override { if (inhibit_fd >= 0) ::close(inhibit_fd); }
    std::string name() const override { return "linux"; }

    std::vector<iio_sensor> scan() const {
        std::vector<iio_sensor> out; std::string root = sys + "/bus/iio/devices";
        for (auto& d : ls(root)) {
            std::string dev = root + "/" + d; auto nm = slurp(dev + "/name");
            for (auto& c : iio_channels(ls(dev))) {
                if (std::any_of(out.begin(), out.end(), [&](const iio_sensor& s) { return s.info.kind == c.kind; })) continue;   // first device wins
                iio_sensor s; s.info = {c.kind, nm.empty() ? d : nm, "iio", 0, 0, 0};
                channel ch{dev, c}; double sc;
                if (chan_attr(ch, c.axes == 3 ? "_x" : "", "_scale", sc)) s.info.resolution = sc * c.mul;
                double hz; if (num(dev + "/" + c.type_prefix + "_sampling_frequency", hz) || num(dev + "/sampling_frequency", hz)) s.info.max_rate_hz = hz;
                s.ch.push_back(std::move(ch)); out.push_back(std::move(s));
            }
        }
        if (!battery_dir().empty()) { iio_sensor b; b.info = {sensor_kind::battery, "power_supply", "linux", 1, 100, 1}; out.push_back(b); }
        return out;
    }
    std::string battery_dir() const {
        std::string r = sys + "/class/power_supply";
        for (auto& e : ls(r)) if (slurp(r + "/" + e + "/type") == "Battery") return r + "/" + e;
        return {};
    }
    bool geoclue_installed() const {
        return real && (exists("/usr/share/dbus-1/system-services/org.freedesktop.GeoClue2.service") || exists("/usr/local/share/dbus-1/system-services/org.freedesktop.GeoClue2.service"));
    }
    result<std::vector<sensor_info>> sensors() override {
        std::vector<sensor_info> v; for (auto& s : scan()) v.push_back(s.info);
        if (geoclue_installed()) v.push_back({sensor_kind::location, "GeoClue2", "freedesktop", 1, 0, 0});
        return {v, {}};
    }

    bool sample(const iio_sensor& s, sensor_event& e) const {
        e = {}; e.kind = s.info.kind; e.t_ns = now_ns();
        if (s.info.kind == sensor_kind::battery) {
            auto d = battery_dir(); double cap; if (d.empty() || !num(d + "/capacity", cap)) return false;
            auto st = slurp(d + "/status"); e.v = {cap, (st == "Charging" || st == "Full") ? 1.0 : 0.0}; e.n = 2; return true;
        }
        return read_chan(s.ch[0], e);
    }
    result<sensor_stream> open_sensor(sensor_kind k, double hz) override {
        if (k == sensor_kind::location) {
            if (!real) return {{}, unsupported("location (GeoClue2 is only used with the live /sys)")};
            auto gc = std::make_shared<geoclue>();
            if (auto st = gc->start(); !st) return {{}, st.err};
            auto q = std::make_shared<queue>(); bridge::get().add_sensor(k, q);
            auto w = std::make_shared<worker>(); auto period = std::max<std::uint64_t>(std::uint64_t(1e9 / hz), 200'000'000ull);
            // The sd-bus connection is used only by this thread from here on (sd-bus objects are not thread-safe).
            w->start([gc, q, wp = w.get(), period] { do { sensor_event e; if (gc->poll(e)) q->push({false, e, {}}); } while (wp->sleep_ns(period)); });
            struct both { std::shared_ptr<worker> w; std::shared_ptr<geoclue> g; };   // stop the thread before Stop()/unref
            auto keep = std::shared_ptr<both>(new both{w, gc}, [](both* b) { b->w.reset(); delete b; });
            return {make_sensor(k, q, keep), {}};
        }
        for (auto& s : scan()) if (s.info.kind == k) {
            auto q = std::make_shared<queue>(); bridge::get().add_sensor(k, q);
            auto w = std::make_shared<worker>(); auto period = std::uint64_t(1e9 / hz);
            w->start([this_s = s, q, wp = w.get(), period, self = shared_from_this()] {
                for (;;) {
                    sensor_event e; if (self->sample(this_s, e)) q->push({false, e, {}});
                    if (!wp->sleep_ns(period)) return;
                }
            });
            return {make_sensor(k, q, w), {}};
        }
        return {{}, unsupported(std::string("sensor ") + sensor_name(k) + " (no IIO device)")};
    }

    // ---- touch: every direct-touch node (touchscreens, not touchpads), positions in display pixels
    std::vector<std::string> touch_nodes() const {
        std::vector<std::string> v;
        for (auto& n : ls(input)) {
            if (n.rfind("event", 0)) continue;
            std::string dev = sys + "/class/input/" + n + "/device";
            std::string abs = slurp(dev + "/capabilities/abs"), key = slurp(dev + "/capabilities/key");
            if (abs.empty()) continue;
            if (!cap_bit(abs, ABS_MT_POSITION_X) && !(cap_bit(abs, ABS_X) && !key.empty() && cap_bit(key, BTN_TOUCH))) continue;
            if (auto props = slurp(dev + "/properties"); !props.empty()) { if (!cap_bit(props, INPUT_PROP_DIRECT)) continue; }
            else { int fd = ::open((input + "/" + n).c_str(), O_RDONLY | O_NONBLOCK | O_CLOEXEC); bool direct = fd >= 0 && evdev_is_direct(fd); if (fd >= 0) ::close(fd); if (!direct) continue; }
            v.push_back(input + "/" + n);
        }
        return v;
    }
    result<touch_stream> open_touch() override {
        auto paths = touch_nodes(); if (paths.empty()) return {{}, unsupported("touch input (no evdev touchscreen)")};
        int w_px = 0, h_px = 0; if (auto d = display()) { w_px = d.value.width_px; h_px = d.value.height_px; }
        std::vector<evdev_node> nodes; error first_err{};
        for (auto& p : paths) {
            int fd = ::open(p.c_str(), O_RDONLY | O_NONBLOCK | O_CLOEXEC);
            if (fd < 0) { if (!first_err) first_err = last_error("open " + p); continue; }
            nodes.push_back(evdev_setup(fd, w_px, h_px));
        }
        if (nodes.empty()) return {{}, first_err};
        auto q = std::make_shared<queue>(); bridge::get().add_touch(q);
        auto w = std::make_shared<worker>();
        w->start([nodes = std::move(nodes), q, wp = w.get()]() mutable { evdev_loop(std::move(nodes), *q, *wp); });
        return {make_touch(q, w), {}};
    }
    // ---- haptics via evdev force feedback
    status vibrate(int ms, double amp) override {
        for (auto& n : ls(input)) {
            if (n.rfind("event", 0)) continue;
            auto ff = slurp(sys + "/class/input/" + n + "/device/capabilities/ff");
            if (ff.empty() || !cap_bit(ff, FF_RUMBLE)) continue;
            int fd = ::open((input + "/" + n).c_str(), O_RDWR | O_CLOEXEC); if (fd < 0) return {{}, last_error("open " + n)};
            ff_effect e{}; e.type = FF_RUMBLE; e.id = -1; e.replay.length = std::uint16_t(ms);
            e.u.rumble.strong_magnitude = std::uint16_t(std::clamp(amp, 0.0, 1.0) * 0xFFFF); e.u.rumble.weak_magnitude = e.u.rumble.strong_magnitude;
            if (::ioctl(fd, EVIOCSFF, &e) < 0) { auto er = last_error("EVIOCSFF"); ::close(fd); return {{}, er}; }
            input_event play{}; play.type = EV_FF; play.code = std::uint16_t(e.id); play.value = 1;
            bool ok = ::write(fd, &play, sizeof play) == ssize_t(sizeof play);
            ::usleep(useconds_t(ms) * 1000); ::ioctl(fd, EVIOCRMFF, e.id); ::close(fd);
            return ok ? status{} : status{{}, last_error("play rumble")};
        }
        return backend::vibrate(ms, amp);
    }

    result<display_info> display() override {
        if (auto b = backend::display()) return b;
        display_info d; std::string drm = sys + "/class/drm";
        for (auto& c : ls(drm)) {
            if (c.find('-') == std::string::npos || slurp(drm + "/" + c + "/status") != "connected") continue;
            auto modes = slurp(drm + "/" + c + "/modes"); auto nl = modes.find('\n'); auto m = modes.substr(0, nl);
            if (std::sscanf(m.c_str(), "%dx%d", &d.width_px, &d.height_px) == 2) break;
        }
        if (!d.width_px) { auto v = slurp(sys + "/class/graphics/fb0/virtual_size"); std::sscanf(v.c_str(), "%d,%d", &d.width_px, &d.height_px); }
        if (!d.width_px) return {{}, unsupported("display metrics (no DRM/fbdev)")};
        d.orient = d.width_px > d.height_px ? orientation::landscape_left : orientation::portrait;
        if (auto o = accel_orientation(); o != orientation::unknown) d.orient = o;
        return {d, {}};
    }
    orientation accel_orientation() const {
        for (auto& s : scan()) if (s.info.kind == sensor_kind::accelerometer) {
            sensor_event e; if (!sample(s, e)) break;
            double x = e.v[0], y = e.v[1]; if (std::hypot(x, y) < 3.0) return orientation::unknown;   // lying flat
            if (std::fabs(y) >= std::fabs(x)) return y >= 0 ? orientation::portrait : orientation::portrait_upside_down;
            return x >= 0 ? orientation::landscape_left : orientation::landscape_right;
        }
        return orientation::unknown;
    }
    result<orientation> screen_orientation() override {
        if (auto o = accel_orientation(); o != orientation::unknown) return {o, {}};
        auto d = display(); if (!d) return {{}, d.err}; return {d.value.orient, {}};
    }
    // Capture nodes only: UVC also registers a metadata node per camera, codecs register m2m nodes. VIDIOC_QUERYCAP on
    // the device node decides; without access to it (fake trees, permissions) the sysfs "index" (0 = the capture
    // interface of a multi-node device) does.
    result<std::vector<camera_info>> cameras() override {
        std::vector<camera_info> v; std::string r = sys + "/class/video4linux";
        std::string devdir = input.substr(0, input.rfind('/'));   // /dev/input -> /dev
        for (auto& c : ls(r)) {
            int capture = -1;
            int fd = ::open((devdir + "/" + c).c_str(), O_RDWR | O_NONBLOCK | O_CLOEXEC);
            if (fd >= 0) {
                v4l2_capability cap{};
                if (::ioctl(fd, VIDIOC_QUERYCAP, &cap) == 0) {
                    std::uint32_t caps = (cap.capabilities & V4L2_CAP_DEVICE_CAPS) ? cap.device_caps : cap.capabilities;
                    capture = (caps & (V4L2_CAP_VIDEO_CAPTURE | V4L2_CAP_VIDEO_CAPTURE_MPLANE)) && !(caps & (V4L2_CAP_VIDEO_M2M | V4L2_CAP_VIDEO_M2M_MPLANE)) ? 1 : 0;
                }
                ::close(fd);
            }
            if (capture < 0) { auto idx = slurp(r + "/" + c + "/index"); capture = idx.empty() || idx == "0"; }
            if (!capture) continue;
            auto nm = slurp(r + "/" + c + "/name"); auto lc = nm; for (auto& ch : lc) ch = char(std::tolower(ch));
            v.push_back({c, nm, lc.find("front") != std::string::npos});
        }
        return {v, {}};
    }
    result<std::vector<std::uint8_t>> capture_photo(std::string_view id) override {
        std::string cam(id);
        if (cam.empty()) { auto c = cameras(); if (!c || c.value.empty()) return {{}, unsupported("camera capture (no V4L2 capture device)")}; cam = c.value[0].id; }
        if (cam.find('/') != std::string::npos || cam.rfind("video", 0) != 0) return {{}, {EINVAL, "capture_photo: camera id is a video4linux node name (video0)", err_domain::posix}};
        return v4l2_capture_jpeg(input.substr(0, input.rfind('/')) + "/" + cam);
    }
    // ---- services
    std::string first_class(const char* cls, const char* contains = nullptr) const {
        std::string r = sys + "/class/" + cls;
        for (auto& e : ls(r)) { if (!contains) return r + "/" + e; auto lc = e; for (auto& c : lc) c = char(std::tolower(c)); if (lc.find(contains) != std::string::npos) return r + "/" + e; }
        return {};
    }
    // The backlight logind and systemd-backlight would pick: firmware (ACPI) over platform over raw.
    std::string backlight_dir() const {
        std::string r = sys + "/class/backlight", best; int rank = 99;
        for (auto& e : ls(r)) {
            auto t = slurp(r + "/" + e + "/type"); int k = t == "firmware" ? 0 : t == "platform" ? 1 : t == "raw" ? 2 : 3;
            if (k < rank) { rank = k; best = r + "/" + e; }
        }
        return best;
    }
    result<double> brightness() override {
        auto d = backlight_dir(); double b, m;
        if (d.empty() || !num(d + "/brightness", b) || !num(d + "/max_brightness", m) || m <= 0) return {{}, unsupported("screen brightness (no backlight)")};
        return {b / m, {}};
    }
    // sysfs needs root (or a udev rule); without write access, logind's Session.SetBrightness does it for the user
    // of the active session.
    status set_brightness(double l) override {
        auto d = backlight_dir(); double m; if (d.empty() || !num(d + "/max_brightness", m)) return {{}, unsupported("screen brightness (no backlight)")};
        auto value = std::uint32_t(std::lround(l * m));
        auto st = write_attr(d + "/brightness", std::to_string(value) + "\n");
        if (st || !real || (st.err.code != EACCES && st.err.code != EPERM && st.err.code != EROFS)) return st;
        bus_conn bus; if (!bus) return st;
        auto& a = sdbus::get(); bus_error e{}; void* reply = nullptr;
        std::string name = d.substr(d.rfind('/') + 1);
        int r = a.call(bus.b, "org.freedesktop.login1", "/org/freedesktop/login1/session/auto", "org.freedesktop.login1.Session", "SetBrightness", &e, &reply, "ssu", "backlight", name.c_str(), value);
        if (reply) a.msg_unref(reply);
        if (r < 0) { auto er = bus_fail(r, e, "logind SetBrightness"); return {{}, is_unsupported(er) ? st.err : er}; }
        a.err_free(&e); return {};
    }
    // logind idle + sleep inhibitor ("block"), held while inhibit_fd is open.
    status keep_awake(bool on) override {
        std::lock_guard g(awake_m);
        if (!on) { if (inhibit_fd >= 0) { ::close(inhibit_fd); inhibit_fd = -1; } return {}; }
        if (inhibit_fd >= 0) return {};
        if (!real) return {{}, unsupported("keep awake (logind is only used with the live /sys)")};
        bus_conn bus; if (!bus) return {{}, unsupported("keep awake (no system D-Bus / libsystemd for logind)")};
        auto& a = sdbus::get(); error last{};
        for (const char* what : {"idle:sleep", "idle"}) {
            bus_error e{}; void* reply = nullptr;
            int r = a.call(bus.b, "org.freedesktop.login1", "/org/freedesktop/login1", "org.freedesktop.login1.Manager", "Inhibit", &e, &reply, "ssss", what, "forge", "keep_awake", "block");
            if (r >= 0 && reply) {
                int fd = -1; int dup = a.read(reply, "h", &fd) >= 0 && fd >= 0 ? ::fcntl(fd, F_DUPFD_CLOEXEC, 3) : -1;   // the reply owns fd
                a.msg_unref(reply); a.err_free(&e);
                if (dup >= 0) { inhibit_fd = dup; return {}; }
                last = {EIO, "logind Inhibit: reply without a file descriptor", err_domain::posix}; continue;
            }
            if (reply) a.msg_unref(reply);
            last = bus_fail(r, e, "logind Inhibit"); if (is_unsupported(last)) break;
        }
        return {{}, last};
    }
    status torch(bool on) override {
        auto d = first_class("leds", "flash"); if (d.empty()) d = first_class("leds", "torch");
        if (d.empty()) return {{}, unsupported("torch (no flash LED)")};
        double m = 1; num(d + "/max_brightness", m);
        return write_attr(d + "/brightness", std::to_string(on ? long(m) : 0L) + "\n");
    }
    result<bool> nfc_available() override { return {!ls(sys + "/class/nfc").empty(), {}}; }
    result<connectivity_info> connectivity() override {
        auto c = backend::connectivity(); if (!c) return c;
        std::string out;   // NetworkManager over its CLI (D-Bus underneath); optional
        if (run_tool({"nmcli", "-t", "-e", "yes", "-f", "ACTIVE,SSID,SIGNAL", "dev", "wifi"}, &out, 5000) == 0) {
            std::size_t i = 0;
            while (i < out.size()) { auto j = out.find('\n', i); auto line = out.substr(i, j - i); i = j == std::string::npos ? out.size() : j + 1;
                auto f = nm_fields(line);
                if (f.size() == 3 && f[0] == "yes") { c.value.ssid = f[1]; c.value.signal_percent = std::atoi(f[2].c_str()); break; } }
        }
        if (c.value.transport == "cellular" || run_tool({"mmcli", "-L"}, &out, 5000) == 0 && out.find("/Modem/") != std::string::npos) {
            if (c.value.transport == "unknown") c.value.transport = "cellular";
        }
        return c;
    }
    // BlueZ through bluetoothctl (D-Bus underneath). Only devices heard during this scan are returned ("[NEW] Device"
    // / "[CHG] Device ... RSSI:" lines), not BlueZ's cache of paired or earlier devices; rssi is 0 when the
    // advertisement carried none.
    result<std::vector<ble_device>> bluetooth_scan(int ms) override {
        std::string out;
        int rc = run_tool({"bluetoothctl", "--timeout", std::to_string(std::max(1, ms / 1000)), "scan", "on"}, &out, ms + 5000);
        if (rc < 0) return {{}, unsupported("bluetooth scan (no bluetoothctl/BlueZ)")};
        if (rc != 0) return {{}, {rc, "bluetoothctl scan failed"}};
        std::vector<ble_device> v; std::size_t i = 0;
        auto find = [&](const std::string& a) -> ble_device& { for (auto& d : v) if (d.address == a) return d; v.push_back({a, "", 0}); return v.back(); };
        while (i < out.size()) {
            auto j = out.find('\n', i); auto line = out.substr(i, j - i); i = j == std::string::npos ? out.size() : j + 1;
            for (std::size_t k; (k = line.find('\x1b')) != std::string::npos;) { auto e = line.find('m', k); line.erase(k, e == std::string::npos ? std::string::npos : e - k + 1); }   // colours
            auto at = line.find("Device "); if (at == std::string::npos || line.size() < at + 24) continue;
            bool is_new = line.find("[NEW]") != std::string::npos, is_chg = line.find("[CHG]") != std::string::npos;
            if (!is_new && !is_chg) continue;
            auto addr = line.substr(at + 7, 17); auto rest = line.size() > at + 25 ? line.substr(at + 25) : std::string();
            if (is_new) { auto& d = find(addr); if (rest != addr && rest.find(':') == std::string::npos) d.name = rest; }
            else if (auto r = rest.find("RSSI: "); r != std::string::npos) {
                auto val = rest.substr(r + 6); auto paren = val.find('(');   // "-60" or "0xffffffc4 (-60)"
                find(addr).rssi = std::atoi((paren != std::string::npos ? val.substr(paren + 1) : val).c_str());
            }
        }
        return {v, {}};
    }
    result<biometric_info> biometrics() override {
        auto user = os::getenv("USER"); if (user.empty() || user[0] == '-') return {biometric_info{false, false, ""}, {}};
        std::string out; int rc = run_tool({"fprintd-list", user}, &out, 5000);
        // Not installed, no reader ("No devices available", exit 1) or fprintd not running: nothing usable.
        if (rc != 0 || out.find("No devices") != std::string::npos) return {biometric_info{false, false, ""}, {}};
        bool enrolled = out.find("Fingerprints for user") != std::string::npos && out.find("has no fingers enrolled") == std::string::npos;
        return {biometric_info{true, enrolled, "fingerprint"}, {}};
    }
    result<device_info> info() override {
        auto d = backend::info(); if (!d) return d;
        // Device tree (ARM boards, phones): model, and the form factor from chassis-type where the DT has it. A DT
        // alone does not make a phone (Raspberry Pi, routers): without chassis-type only a battery-powered device with
        // a touchscreen is taken for a phone, a battery alone for a laptop.
        auto dt = sys + "/firmware/devicetree/base";
        auto strip = [](std::string v) { while (!v.empty() && v.back() == '\0') v.pop_back(); return v; };
        std::string model = strip(slurp(dt + "/model"));
        if (!model.empty()) {
            d.value.model = model; auto ct = strip(slurp(dt + "/chassis-type"));
            d.value.form = ct == "handset" ? form_factor::phone : ct == "tablet" ? form_factor::tablet : ct == "watch" ? form_factor::watch
                         : ct == "laptop" || ct == "convertible" ? form_factor::laptop : ct == "desktop" || ct == "all-in-one" ? form_factor::desktop
                         : ct == "television" ? form_factor::tv : ct == "embedded" ? form_factor::unknown
                         : !battery_dir().empty() ? (touch_nodes().empty() ? form_factor::laptop : form_factor::phone) : form_factor::unknown;
        }
        auto dmi = sys + "/class/dmi/id";
        if (auto v = slurp(dmi + "/sys_vendor"); !v.empty()) d.value.manufacturer = v;
        if (auto v = slurp(dmi + "/product_name"); !v.empty() && model.empty()) d.value.model = v;
        if (auto ct = slurp(dmi + "/chassis_type"); !ct.empty()) {
            int t = std::atoi(ct.c_str());
            d.value.form = (t == 8 || t == 9 || t == 10 || t == 14 || t == 31) ? form_factor::laptop : (t == 30 || t == 32) ? form_factor::tablet : form_factor::desktop;
        }
        return d;
    }
    result<audio_clip> record_audio(int ms, int rate) override {   // ALSA through arecord
        std::string out;
        int rc = run_tool({"arecord", "-q", "-t", "raw", "-f", "S16_LE", "-c", "1", "-r", std::to_string(rate), "-d", std::to_string(std::max(1, (ms + 999) / 1000))}, &out, ms + 10000);
        if (rc < 0) return {{}, unsupported("audio recording (no arecord)")};
        if (rc != 0) return {{}, {rc, "arecord failed"}};
        audio_clip c; c.sample_rate = rate; c.samples.resize(std::min<std::size_t>(out.size() / 2, std::size_t(rate) * std::size_t(ms) / 1000));
        std::memcpy(c.samples.data(), out.data(), c.samples.size() * 2); return {c, {}};
    }
    status play_audio(const audio_clip& c) override {
        spawn_spec sp; sp.argv = {"aplay", "-q", "-t", "raw", "-f", "S16_LE", "-c", std::to_string(c.channels), "-r", std::to_string(c.sample_rate)};
        sp.stdin_data = std::string(reinterpret_cast<const char*>(c.samples.data()), c.samples.size() * 2);
        auto r = spawn_wait(sp); if (!r || r.value.exit_code == 127) return {{}, unsupported("audio playback (no aplay)")};
        return r.value.exit_code == 0 ? status{} : status{{}, {r.value.exit_code, "aplay failed"}};
    }
    result<permission_state> permission_status(permission p) override { if (auto b = backend::permission_status(p)) return b; return {permission_state::not_required, {}}; }
    status request_permission(permission) override { return {}; }
    status notify(std::string_view t, std::string_view b) override {
        if (auto h = backend::notify(t, b); !is_unsupported(h.err)) return h;
        std::string ts(t), bs(b); char* argv[] = {const_cast<char*>("notify-send"), const_cast<char*>("--app-name=forge"), const_cast<char*>("--"), ts.data(), bs.data(), nullptr};
        pid_t pid; if (posix_spawnp(&pid, "notify-send", nullptr, nullptr, argv, environ) != 0) return {{}, unsupported("notifications (no notify-send)")};
        int st = 0; ::waitpid(pid, &st, 0);
        return WIFEXITED(st) && WEXITSTATUS(st) == 0 ? status{} : status{{}, {-1, "notify-send failed"}};
    }
};
} // namespace
std::shared_ptr<backend> make_linux(std::string sysfs, std::string input) { return std::make_shared<linux_backend>(std::move(sysfs), std::move(input)); }
} // namespace forge::os::mobile::detail
#endif
