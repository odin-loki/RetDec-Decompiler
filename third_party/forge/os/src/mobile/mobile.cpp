// forge_mobile: shared pieces (names, queues, streams, gestures, device, bridge), null and simulated backends.
#include "internal.hpp"
#include <forge/json/json.hpp>
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <sstream>

namespace forge::os::mobile {
using namespace detail;

// ------------------------------------------------------------------ names
static constexpr const char* k_sensor[] = {"accelerometer", "gyroscope", "magnetometer", "light", "proximity", "pressure", "location", "battery",
    "step_counter", "step_detector", "rotation_vector", "gravity", "linear_acceleration", "heart_rate", "temperature", "humidity"};
const char* sensor_name(sensor_kind k) { return k_sensor[int(k)]; }
std::optional<sensor_kind> parse_sensor(std::string_view s) { for (int i = 0; i < sensor_kind_count; ++i) if (s == k_sensor[i]) return sensor_kind(i); return std::nullopt; }
static constexpr const char* k_phase[] = {"down", "move", "up", "cancel"};
const char* touch_phase_name(touch_phase p) { return k_phase[int(p)]; }
static std::optional<touch_phase> parse_phase(std::string_view s) { for (int i = 0; i < 4; ++i) if (s == k_phase[i]) return touch_phase(i); return std::nullopt; }
static constexpr const char* k_gesture[] = {"tap", "double_tap", "long_press", "swipe", "pinch", "rotate"};
const char* gesture_name(gesture_kind g) { return k_gesture[int(g)]; }
static constexpr const char* k_orient[] = {"unknown", "portrait", "portrait_upside_down", "landscape_left", "landscape_right"};
const char* orientation_name(orientation o) { return k_orient[int(o)]; }
std::optional<orientation> parse_orientation(std::string_view s) {
    if (s == "landscape") return orientation::landscape_left;
    for (int i = 0; i < 5; ++i) if (s == k_orient[i]) return orientation(i); return std::nullopt;
}
static constexpr const char* k_perm[] = {"sensors", "location", "camera", "notifications", "vibrate"};
const char* permission_name(permission p) { return k_perm[int(p)]; }
std::optional<permission> parse_permission(std::string_view s) { for (int i = 0; i < 5; ++i) if (s == k_perm[i]) return permission(i); return std::nullopt; }
static constexpr const char* k_pstate[] = {"granted", "denied", "not_determined", "not_required"};
const char* permission_state_name(permission_state s) { return k_pstate[int(s)]; }
static std::optional<permission_state> parse_pstate(std::string_view s) { for (int i = 0; i < 4; ++i) if (s == k_pstate[i]) return permission_state(i); return std::nullopt; }

namespace detail {
std::pmr::memory_resource* mobile_resource() {
    static forge::mem::budget_resource r(forge::mem::host_budget(), "mobile");
    return &r;
}
bridge& bridge::get() { static bridge b; return b; }

status backend::vibrate(int ms, double amp) {
    auto& br = bridge::get(); std::function<bool(int, double)> f; { std::lock_guard g(br.m); f = br.hooks.vibrate; }
    if (f) return f(ms, amp) ? status{} : status{{}, {-1, "vibrate: app hook failed"}};
    return {{}, unsupported("vibration")};
}
result<display_info> backend::display() {
    auto& br = bridge::get(); std::lock_guard g(br.m);
    if (br.disp) return {*br.disp, {}};
    return {{}, unsupported("display metrics")};
}
result<permission_state> backend::permission_status(permission p) {
    auto& br = bridge::get(); std::lock_guard g(br.m);
    if (auto& s = br.perms[int(p)]) return {*s, {}};
    return {{}, unsupported(std::string("permission ") + permission_name(p))};
}
status backend::request_permission(permission p) {
    auto& br = bridge::get(); std::function<bool(permission)> f; { std::lock_guard g(br.m); f = br.hooks.request_permission; }
    if (f) return f(p) ? status{} : status{{}, {-1, "request_permission: app hook failed"}};
    return {{}, unsupported("permission requests")};
}
status backend::notify(std::string_view t, std::string_view b) {
    auto& br = bridge::get(); std::function<bool(std::string_view, std::string_view)> f; { std::lock_guard g(br.m); f = br.hooks.notify; }
    if (f) return f(t, b) ? status{} : status{{}, {-1, "notify: app hook failed"}};
    return {{}, unsupported("notifications")};
}
} // namespace detail

// ------------------------------------------------------------------ bridge entry points
void feed_touch(const touch_event& e) {
    auto& br = bridge::get(); std::lock_guard g(br.m);
    std::erase_if(br.touch, [&](auto& w) { auto q = w.lock(); if (!q) return true; q->push({true, {}, e}); return false; });
}
void feed_sensor(const sensor_event& e) {
    auto& br = bridge::get(); std::lock_guard g(br.m);
    std::erase_if(br.sensor, [&](auto& p) { auto q = p.second.lock(); if (!q) return true; if (p.first == e.kind) q->push({false, e, {}}); return false; });
}
void report_display(const display_info& d) { auto& br = bridge::get(); std::lock_guard g(br.m); br.disp = d; }
void report_permission(permission p, permission_state s) { auto& br = bridge::get(); std::lock_guard g(br.m); br.perms[int(p)] = s; }
void provide_sensor(sensor_kind k, bool on) { auto& br = bridge::get(); std::lock_guard g(br.m); br.provided[std::size_t(k)] = on; }
void set_action_hooks(action_hooks h) { auto& br = bridge::get(); std::lock_guard g(br.m); br.hooks = std::move(h); }

// ------------------------------------------------------------------ streams
sensor_stream::sensor_stream(sensor_stream&&) noexcept = default;
sensor_stream& sensor_stream::operator=(sensor_stream&&) noexcept = default;
sensor_stream::~sensor_stream() { keep_.reset(); }   // stop the producer before the queue goes
std::vector<sensor_event> sensor_stream::poll(int timeout_ms, std::size_t max) {
    std::vector<sensor_event> v; if (!q_) return v;
    q_->drain(timeout_ms, max, [&](item& it) { if (!it.touch) v.push_back(it.s); }); return v;
}
std::uint64_t sensor_stream::dropped() const { return q_ ? q_->dropped.load() : 0; }
bool sensor_stream::finished() const { if (!q_) return true; std::lock_guard g(q_->m); return q_->finished && q_->q.empty(); }

touch_stream::touch_stream(touch_stream&&) noexcept = default;
touch_stream& touch_stream::operator=(touch_stream&&) noexcept = default;
touch_stream::~touch_stream() { keep_.reset(); }
std::vector<touch_event> touch_stream::poll(int timeout_ms, std::size_t max) {
    std::vector<touch_event> v; if (!q_) return v;
    q_->drain(timeout_ms, max, [&](item& it) { if (it.touch) v.push_back(it.t); }); return v;
}
std::vector<gesture> touch_stream::gestures(int timeout_ms) {
    std::vector<gesture> out;
    for (auto& e : poll(timeout_ms)) for (auto& g : rec_.feed(e)) out.push_back(g);
    return out;
}
std::uint64_t touch_stream::dropped() const { return q_ ? q_->dropped.load() : 0; }
bool touch_stream::finished() const { if (!q_) return true; std::lock_guard g(q_->m); return q_->finished && q_->q.empty(); }

// ------------------------------------------------------------------ gestures
std::vector<gesture> gesture_recognizer::feed(const touch_event& e) {
    std::vector<gesture> out;
    auto find = [&](int id) { return std::find_if(live_.begin(), live_.end(), [&](const pt& p) { return p.id == id; }); };
    auto dist2 = [&] { double dx = live_[0].x - live_[1].x, dy = live_[0].y - live_[1].y; return std::sqrt(dx * dx + dy * dy); };
    switch (e.phase) {
    case touch_phase::down:
        if (auto it = find(e.id); it != live_.end()) live_.erase(it);
        live_.push_back({e.id, e.x, e.y, e.x, e.y, e.t_ns, false});
        if (live_.size() == 2) { multi_ = true; pinch0_ = dist2(); pinch_last_ = 1; angle0_ = std::atan2(live_[1].y - live_[0].y, live_[1].x - live_[0].x); angle_last_ = 0; }
        break;
    case touch_phase::move: {
        auto it = find(e.id); if (it == live_.end()) break;
        it->x = e.x; it->y = e.y;
        if (live_.size() == 2 && pinch0_ > 0) {
            double s = dist2() / pinch0_;
            double cx = (live_[0].x + live_[1].x) / 2, cy = (live_[0].y + live_[1].y) / 2;
            if (std::fabs(s - pinch_last_) >= 0.05) { pinch_last_ = s; out.push_back({gesture_kind::pinch, e.t_ns, cx, cy, 0, 0, s}); }
            constexpr double pi = 3.14159265358979323846;
            double a = std::atan2(live_[1].y - live_[0].y, live_[1].x - live_[0].x) - angle0_;
            while (a > pi) a -= 2 * pi; while (a < -pi) a += 2 * pi;
            if (std::fabs(a - angle_last_) >= 0.1) { angle_last_ = a; out.push_back({gesture_kind::rotate, e.t_ns, cx, cy, 0, 0, s, a}); }
        } else if (!multi_ && !it->long_fired && std::hypot(it->x - it->x0, it->y - it->y0) <= c_.tap_slop_px && e.t_ns - it->t0 >= c_.long_press_ns) {
            it->long_fired = true; out.push_back({gesture_kind::long_press, e.t_ns, it->x, it->y});
        }
        break;
    }
    case touch_phase::up: {
        auto it = find(e.id); if (it == live_.end()) break;
        it->x = e.x; it->y = e.y;
        double dx = it->x - it->x0, dy = it->y - it->y0, d = std::hypot(dx, dy); std::uint64_t dt = e.t_ns - it->t0;
        if (!multi_ && !it->long_fired) {
            if (d >= c_.swipe_min_px) out.push_back({gesture_kind::swipe, e.t_ns, it->x0, it->y0, dx, dy});
            else if (d <= c_.tap_slop_px && dt >= c_.long_press_ns) out.push_back({gesture_kind::long_press, e.t_ns, it->x, it->y});
            else if (d <= c_.tap_slop_px && dt <= c_.tap_max_ns) {
                out.push_back({gesture_kind::tap, e.t_ns, it->x, it->y});
                if (have_tap_ && e.t_ns - last_tap_t_ <= c_.double_tap_ns && std::hypot(it->x - last_tap_x_, it->y - last_tap_y_) <= c_.tap_slop_px * 2) {
                    out.push_back({gesture_kind::double_tap, e.t_ns, it->x, it->y}); have_tap_ = false;
                } else { have_tap_ = true; last_tap_t_ = e.t_ns; last_tap_x_ = it->x; last_tap_y_ = it->y; }
            }
        }
        live_.erase(it);
        if (live_.size() < 2) pinch0_ = 0;
        if (live_.empty()) multi_ = false;
        break;
    }
    case touch_phase::cancel: live_.clear(); multi_ = false; pinch0_ = 0; break;
    }
    return out;
}

// ------------------------------------------------------------------ null backend
namespace detail {
struct null_backend final : backend { std::string name() const override { return "null"; } };
std::shared_ptr<backend> make_null() { return std::make_shared<null_backend>(); }

// ------------------------------------------------------------------ simulated backend (JSON trace)
struct sim_backend final : backend, std::enable_shared_from_this<sim_backend> {
    struct ev { std::uint64_t t_ns; item it; std::optional<orientation> orient; std::optional<app_event> life; };
    json::value services = json::object{}; double bright = 0.5; std::string clip; std::shared_ptr<worker> life_w; bool life_started = false;
    bool realtime = false;
    std::vector<sensor_info> sens; std::vector<camera_info> cams; std::optional<display_info> disp;
    std::array<permission_state, 5> perms{permission_state::granted, permission_state::granted, permission_state::granted, permission_state::granted, permission_state::granted};
    std::vector<ev> events; std::uint64_t t0 = now_ns();
    mutable std::mutex m; std::vector<sim_record> log;

    std::string name() const override { return "sim"; }
    result<std::vector<sensor_info>> sensors() override { return {sens, {}}; }
    bool has(sensor_kind k) const { return std::any_of(sens.begin(), sens.end(), [&](auto& s) { return s.kind == k; }); }
    template <class Pred> std::shared_ptr<worker> replay(std::shared_ptr<queue> q, Pred pred) {
        auto w = std::make_shared<worker>(); auto self = shared_from_this();
        w->start([self, q, pred, wp = w.get()] {
            std::uint64_t start = now_ns(), base = self->events.empty() ? 0 : self->events.front().t_ns;
            for (auto& e : self->events) {
                if (wp->stop) return;
                if (!pred(e.it) || e.orient || e.life) continue;
                if (self->realtime) { auto due = start + (e.t_ns - base), n = now_ns(); if (due > n && !wp->sleep_ns(due - n)) return; }
                q->push(e.it);
            }
            q->finish();
        });
        return w;
    }
    result<sensor_stream> open_sensor(sensor_kind k, double) override {
        if (!has(k)) return {{}, unsupported(std::string("sensor ") + sensor_name(k) + " (not in trace)")};
        { std::lock_guard g(m); if (perms[k == sensor_kind::location ? 1 : 0] == permission_state::denied) return {{}, {13, std::string("sensor ") + sensor_name(k) + ": permission denied"}}; }
        auto q = std::make_shared<queue>(); bridge::get().add_sensor(k, q);
        auto w = replay(q, [k](const item& it) { return !it.touch && it.s.kind == k; });
        return {make_sensor(k, q, w), {}};
    }
    result<touch_stream> open_touch() override {
        auto q = std::make_shared<queue>(); bridge::get().add_touch(q);
        auto w = replay(q, [](const item& it) { return it.touch; });
        return {make_touch(q, w), {}};
    }
    status vibrate(int ms, double amp) override {
        std::lock_guard g(m);
        if (perms[int(permission::vibrate)] == permission_state::denied) return {{}, {13, "vibrate: permission denied"}};
        log.push_back({"vibrate", std::to_string(ms) + "ms@" + std::to_string(amp)}); return {};
    }
    result<display_info> display() override {
        if (!disp) return {{}, unsupported("display metrics (not in trace)")};
        display_info d = *disp; std::uint64_t el = now_ns() - t0, base = events.empty() ? 0 : events.front().t_ns;
        for (auto& e : events) if (e.orient && (!realtime || e.t_ns - base <= el)) d.orient = *e.orient;
        if ((d.orient == orientation::landscape_left || d.orient == orientation::landscape_right) != (d.width_px > d.height_px)) std::swap(d.width_px, d.height_px);
        return {d, {}};
    }
    result<std::vector<camera_info>> cameras() override { return {cams, {}}; }
    result<permission_state> permission_status(permission p) override { std::lock_guard g(m); return {perms[int(p)], {}}; }
    status request_permission(permission p) override {
        std::lock_guard g(m); log.push_back({"request_permission", permission_name(p)});
        if (perms[int(p)] == permission_state::not_determined) perms[int(p)] = permission_state::granted;
        return perms[int(p)] == permission_state::denied ? status{{}, {13, std::string(permission_name(p)) + ": permission denied"}} : status{};
    }
    status notify(std::string_view t, std::string_view b) override {
        std::lock_guard g(m);
        if (perms[int(permission::notifications)] == permission_state::denied) return {{}, {13, "notify: permission denied"}};
        log.push_back({"notify", std::string(t) + ": " + std::string(b)}); return {};
    }
    std::vector<sim_record> sim_log() const override { std::lock_guard g(m); return log; }
    ~sim_backend() override { life_w.reset(); }
    std::optional<std::string> svc(std::string_view op, std::string_view args) override {
        std::lock_guard g(m);
        auto deny = [&](permission p) { return perms[int(p)] == permission_state::denied; };
        auto act = [&](std::optional<permission> p = std::nullopt) -> std::optional<std::string> {
            if (p && deny(*p)) return std::string("{\"error\":\"") + std::string(op) + ": permission denied\",\"code\":13}";
            log.push_back({std::string(op), std::string(args)}); return std::string("true");
        };
        if (op == "clipboard_get") return json::dump(json::value(clip));
        if (op == "clipboard_set") { clip = json::parse(args)["text"].str(); return act(); }
        if (op == "brightness") return std::to_string(bright);
        if (op == "set_brightness") { bright = json::parse(args)["level"].num(); return act(); }
        if (op == "torch" || op == "capture_photo") {
            if (cams.empty()) return std::string("{\"unsupported\":\"camera (none in trace)\"}");
            if (op == "torch") return act(permission::camera);
            if (deny(permission::camera)) return act(permission::camera);
            log.push_back({"capture_photo", std::string(args)});
            if (auto* p = services.find("photo")) return json::dump(*p);
            return std::string("{\"bytes\":[255,216,255,224,0,16,74,70,73,70,0,1,255,217]}");   // minimal JFIF SOI..EOI
        }
        if (op == "record_audio") { if (auto r = act(); *r != "true") return r; if (auto* a = services.find("audio")) return json::dump(*a); return std::string("{\"unsupported\":\"audio recording (no audio in trace)\"}"); }
        if (op == "play_audio") { auto v = json::parse(args); log.push_back({"play_audio", std::to_string(v["samples"].arr().size()) + " samples @" + std::to_string(int(v.num_or("sample_rate", 0)))}); return std::string("true"); }
        if (op == "keep_awake" || op == "lock_orientation" || op == "show_keyboard" || op == "open_url" || op == "share") return act();
        if (op == "haptic_pattern") return act(permission::vibrate);
        if (op == "bluetooth_scan" || op == "gnss") { if (deny(permission::location)) return act(permission::location); }
        if (auto* v = services.find(op)) return json::dump(*v);
        return std::string("{\"unsupported\":\"") + std::string(op) + " (not in trace)\"}";
    }
    void lifecycle_started() override {
        std::lock_guard g(m); if (life_started) return; life_started = true;
        life_w = std::make_shared<worker>();
        life_w->start([this, wp = life_w.get()] {
            std::uint64_t start = now_ns(), base = events.empty() ? 0 : events.front().t_ns;
            for (auto& e : events) {
                if (!e.life) continue;
                if (realtime) { auto due = start + (e.t_ns - base), n = now_ns(); if (due > n && !wp->sleep_ns(due - n)) return; }
                if (wp->stop) return;
                dispatch_lifecycle(*e.life);
            }
        });
    }
};

std::shared_ptr<backend> make_sim(std::string_view text, bool realtime, error& err) {
    auto b = std::make_shared<sim_backend>(); b->realtime = realtime;
    try {
        auto j = json::parse(text);
        if (auto* d = j.find("display")) {
            display_info di; di.width_px = int(d->num_or("width", 0)); di.height_px = int(d->num_or("height", 0));
            di.dpi = d->num_or("dpi", 0); di.scale = d->num_or("scale", di.dpi > 0 ? di.dpi / 160.0 : 1); di.refresh_hz = d->num_or("refresh_hz", 60);
            di.orient = parse_orientation(d->str_or("orientation", "portrait")).value_or(orientation::unknown);
            b->disp = di;
        }
        if (auto* s = j.find("sensors")) for (auto& x : s->arr()) {
            sensor_info si; auto k = parse_sensor(x.is_str() ? x.str() : x["kind"].str());
            if (!k) { err = {22, "trace: unknown sensor kind"}; return nullptr; }
            si.kind = *k; si.name = x.is_obj() ? x.str_or("name", std::string("sim-") + sensor_name(*k)) : std::string("sim-") + sensor_name(*k);
            si.vendor = "forge-sim"; si.max_rate_hz = x.is_obj() ? x.num_or("max_rate_hz", 200) : 200; b->sens.push_back(si);
        }
        if (auto* c = j.find("cameras")) for (auto& x : c->arr()) b->cams.push_back({x.str_or("id", "0"), x.str_or("name", "sim-camera"), x.find("front") && x["front"].b()});
        if (auto* p = j.find("permissions")) for (auto& [k, v] : p->obj()) {
            auto pk = parse_permission(k); auto ps = parse_pstate(v.str());
            if (!pk || !ps) { err = {22, "trace: bad permission entry '" + k + "'"}; return nullptr; }
            b->perms[int(*pk)] = *ps;
        }
        if (auto* sv = j.find("services")) { if (!sv->is_obj()) { err = {22, "trace: services must be an object"}; return nullptr; } b->services = *sv;
            if (auto* c = sv->find("clipboard")) b->clip = c->str(); if (auto* br = sv->find("brightness")) b->bright = br->num(); }
        if (auto* e = j.find("events")) for (auto& x : e->arr()) {
            sim_backend::ev ev{}; ev.t_ns = std::uint64_t(x.num_or("t_ms", 0) * 1e6);
            auto type = x.str_or("type", "");
            if (type == "sensor") {
                auto k = parse_sensor(x["kind"].str()); if (!k) { err = {22, "trace: unknown sensor '" + x["kind"].str() + "'"}; return nullptr; }
                ev.it.s.kind = *k; ev.it.s.t_ns = ev.t_ns; auto& v = x["v"].arr(); ev.it.s.n = int(std::min<std::size_t>(v.size(), 4));
                for (int i = 0; i < ev.it.s.n; ++i) ev.it.s.v[i] = v[i].num();
                if (!b->has(*k)) b->sens.push_back({*k, std::string("sim-") + sensor_name(*k), "forge-sim", 200, 0, 0});
            } else if (type == "touch") {
                auto ph = parse_phase(x.str_or("phase", "")); if (!ph) { err = {22, "trace: bad touch phase"}; return nullptr; }
                ev.it.touch = true; ev.it.t = {ev.t_ns, *ph, int(x.num_or("id", 0)), x.num_or("x", 0), x.num_or("y", 0), x.num_or("pressure", 1), int(x.num_or("pointers", 1))};
            } else if (type == "orientation") {
                auto o = parse_orientation(x.str_or("value", "")); if (!o) { err = {22, "trace: bad orientation"}; return nullptr; }
                ev.orient = *o;
            } else if (type == "lifecycle") {
                auto l = parse_app_event(x.str_or("value", "")); if (!l) { err = {22, "trace: bad lifecycle value"}; return nullptr; }
                ev.life = *l;
            } else { err = {22, "trace: unknown event type '" + type + "'"}; return nullptr; }
            b->events.push_back(ev);
        }
        std::stable_sort(b->events.begin(), b->events.end(), [](auto& a, auto& c) { return a.t_ns < c.t_ns; });
    } catch (const std::exception& ex) { err = {22, std::string("trace: ") + ex.what()}; return nullptr; }
    return b;
}
} // namespace detail

// ------------------------------------------------------------------ device
device::device(device&&) noexcept = default;
device& device::operator=(device&&) noexcept = default;
device::~device() = default;

result<device> device::null_device() { device d; d.b_ = make_null(); return {std::move(d), {}}; }
result<device> device::simulated(std::string_view json, bool realtime) {
    error e; auto b = make_sim(json, realtime, e); if (!b) return {{}, e};
    device d; d.b_ = std::move(b); return {std::move(d), {}};
}
result<device> device::simulated_file(const std::string& path, bool realtime) {
    std::ifstream f(path, std::ios::binary); if (!f) return {{}, {2, "trace: cannot open " + path}};
    std::stringstream ss; ss << f.rdbuf(); return simulated(ss.str(), realtime);
}
#if defined(__linux__)
result<device> device::linux_with(std::string sysfs, std::string input) {
#if defined(__ANDROID__)
    (void)sysfs; (void)input; return {{}, unsupported("linux_with on Android")};
#else
    device d; d.b_ = make_linux(std::move(sysfs), std::move(input)); return {std::move(d), {}};
#endif
}
#endif
result<device> device::open() {
    if (const char* t = std::getenv("FORGE_MOBILE_TRACE"); t && *t) return simulated_file(t, true);
    device d;
#if defined(__ANDROID__)
    d.b_ = make_android();
#elif defined(__APPLE__)
    d.b_ = make_apple();
#elif defined(_WIN32)
    d.b_ = make_windows();
#elif defined(__FreeBSD__)
    d.b_ = make_freebsd();
#elif defined(__linux__)
    const char* s = std::getenv("FORGE_MOBILE_SYSFS"); const char* i = std::getenv("FORGE_MOBILE_INPUT");
    d.b_ = make_linux(s && *s ? s : "/sys", i && *i ? i : "/dev/input");
#else
    d.b_ = make_null();
#endif
    return {std::move(d), {}};
}

#define FORGE_MOBILE_NEED(ret) if (!b_) return {{}, {22, "mobile: empty device"}}
std::string device::backend_name() const { return b_ ? b_->name() : "none"; }
result<std::vector<sensor_info>> device::sensors() {
    FORGE_MOBILE_NEED(); auto r = b_->sensors();
    if (!r && !is_unsupported(r.err)) return r;
    if (!r) r = {{}, {}};
    auto& br = bridge::get(); std::lock_guard g(br.m);
    for (int k = 0; k < sensor_kind_count; ++k)
        if (br.provided[std::size_t(k)] && std::none_of(r.value.begin(), r.value.end(), [&](auto& s) { return s.kind == sensor_kind(k); }))
            r.value.push_back({sensor_kind(k), std::string("app:") + sensor_name(sensor_kind(k)), "feed_sensor", 0, 0, 0});
    return r;
}
bool device::has_sensor(sensor_kind k) { auto s = sensors(); return s && std::any_of(s.value.begin(), s.value.end(), [&](auto& x) { return x.kind == k; }); }
result<sensor_stream> device::open_sensor(sensor_kind k, double hz) {
    FORGE_MOBILE_NEED(); if (!(hz > 0) || hz > 10000) return {{}, {22, "open_sensor: rate_hz must be in (0, 10000]"}};
    auto r = b_->open_sensor(k, hz);
    if (r || !is_unsupported(r.err)) return r;
    // No native source: a stream fed only by the app glue, when the glue said it provides this kind.
    auto& br = bridge::get(); { std::lock_guard g(br.m); if (!br.provided[std::size_t(k)]) return r; }
    auto q = std::make_shared<queue>(); br.add_sensor(k, q);
    return {backend::make_sensor(k, q, nullptr), {}};
}
result<touch_stream> device::open_touch() { FORGE_MOBILE_NEED(); return b_->open_touch(); }
status device::vibrate(int ms, double amp) { FORGE_MOBILE_NEED(); if (ms <= 0 || ms > 60000 || amp < 0 || amp > 1) return {{}, {22, "vibrate: ms in (0, 60000], amplitude in [0, 1]"}}; return b_->vibrate(ms, amp); }
result<display_info> device::display() { FORGE_MOBILE_NEED(); return b_->display(); }
result<orientation> device::screen_orientation() { FORGE_MOBILE_NEED(); return b_->screen_orientation(); }
result<std::vector<camera_info>> device::cameras() { FORGE_MOBILE_NEED(); return b_->cameras(); }
result<permission_state> device::permission_status(permission p) { FORGE_MOBILE_NEED(); return b_->permission_status(p); }
status device::request_permission(permission p) { FORGE_MOBILE_NEED(); return b_->request_permission(p); }
status device::notify(std::string_view t, std::string_view body) { FORGE_MOBILE_NEED(); return b_->notify(t, body); }
std::vector<sim_record> device::sim_log() const { return b_ ? b_->sim_log() : std::vector<sim_record>{}; }

// ------------------------------------------------------------------ pure helpers shared by the backends
namespace detail {
std::vector<float> remix(const float* in, std::size_t frames, int in_ch, int in_rate, int out_ch, int out_rate) {
    if (!in || frames == 0 || in_ch < 1 || out_ch < 1 || in_rate <= 0 || out_rate <= 0) return {};
    std::vector<float> mixed(frames * std::size_t(out_ch));
    for (std::size_t f = 0; f < frames; ++f) {
        const float* s = in + f * std::size_t(in_ch); float* d = mixed.data() + f * std::size_t(out_ch);
        if (out_ch == in_ch) std::copy(s, s + in_ch, d);
        else if (out_ch == 1) { float a = 0; for (int c = 0; c < in_ch; ++c) a += s[c]; d[0] = a / float(in_ch); }
        else for (int c = 0; c < out_ch; ++c) d[c] = in_ch == 1 ? s[0] : c < in_ch ? s[c] : 0.0f;
    }
    if (in_rate == out_rate) return mixed;
    std::size_t out_frames = std::size_t((double(frames) * out_rate + in_rate / 2) / in_rate); if (out_frames == 0) out_frames = 1;
    std::vector<float> out(out_frames * std::size_t(out_ch));
    double step = double(in_rate) / out_rate;
    for (std::size_t f = 0; f < out_frames; ++f) {
        double pos = double(f) * step; std::size_t i = std::size_t(pos); double t = pos - double(i);
        if (i >= frames - 1) { i = frames - 1; t = 0; }
        for (int c = 0; c < out_ch; ++c) {
            float a = mixed[i * std::size_t(out_ch) + std::size_t(c)], b = t > 0 ? mixed[(i + 1) * std::size_t(out_ch) + std::size_t(c)] : a;
            out[f * std::size_t(out_ch) + std::size_t(c)] = float(a + (b - a) * t);
        }
    }
    return out;
}

// ITU T.81 Annex K.3 tables (also libjpeg's std_huff_tables): class/id, 16 code-length counts, values.
static const std::uint8_t k_dc_bits[2][16] = {{0, 1, 5, 1, 1, 1, 1, 1, 1, 0, 0, 0, 0, 0, 0, 0}, {0, 3, 1, 1, 1, 1, 1, 1, 1, 1, 1, 0, 0, 0, 0, 0}};
static const std::uint8_t k_ac_bits[2][16] = {{0, 2, 1, 3, 3, 2, 4, 3, 5, 5, 4, 4, 0, 0, 1, 0x7d}, {0, 2, 1, 2, 4, 4, 3, 4, 7, 5, 4, 4, 0, 1, 2, 0x77}};
static const std::uint8_t k_ac_vals[2][162] = {
    {0x01, 0x02, 0x03, 0x00, 0x04, 0x11, 0x05, 0x12, 0x21, 0x31, 0x41, 0x06, 0x13, 0x51, 0x61, 0x07, 0x22, 0x71, 0x14, 0x32, 0x81, 0x91, 0xa1, 0x08,
     0x23, 0x42, 0xb1, 0xc1, 0x15, 0x52, 0xd1, 0xf0, 0x24, 0x33, 0x62, 0x72, 0x82, 0x09, 0x0a, 0x16, 0x17, 0x18, 0x19, 0x1a, 0x25, 0x26, 0x27, 0x28,
     0x29, 0x2a, 0x34, 0x35, 0x36, 0x37, 0x38, 0x39, 0x3a, 0x43, 0x44, 0x45, 0x46, 0x47, 0x48, 0x49, 0x4a, 0x53, 0x54, 0x55, 0x56, 0x57, 0x58, 0x59,
     0x5a, 0x63, 0x64, 0x65, 0x66, 0x67, 0x68, 0x69, 0x6a, 0x73, 0x74, 0x75, 0x76, 0x77, 0x78, 0x79, 0x7a, 0x83, 0x84, 0x85, 0x86, 0x87, 0x88, 0x89,
     0x8a, 0x92, 0x93, 0x94, 0x95, 0x96, 0x97, 0x98, 0x99, 0x9a, 0xa2, 0xa3, 0xa4, 0xa5, 0xa6, 0xa7, 0xa8, 0xa9, 0xaa, 0xb2, 0xb3, 0xb4, 0xb5, 0xb6,
     0xb7, 0xb8, 0xb9, 0xba, 0xc2, 0xc3, 0xc4, 0xc5, 0xc6, 0xc7, 0xc8, 0xc9, 0xca, 0xd2, 0xd3, 0xd4, 0xd5, 0xd6, 0xd7, 0xd8, 0xd9, 0xda, 0xe1, 0xe2,
     0xe3, 0xe4, 0xe5, 0xe6, 0xe7, 0xe8, 0xe9, 0xea, 0xf1, 0xf2, 0xf3, 0xf4, 0xf5, 0xf6, 0xf7, 0xf8, 0xf9, 0xfa},
    {0x00, 0x01, 0x02, 0x03, 0x11, 0x04, 0x05, 0x21, 0x31, 0x06, 0x12, 0x41, 0x51, 0x07, 0x61, 0x71, 0x13, 0x22, 0x32, 0x81, 0x08, 0x14, 0x42, 0x91,
     0xa1, 0xb1, 0xc1, 0x09, 0x23, 0x33, 0x52, 0xf0, 0x15, 0x62, 0x72, 0xd1, 0x0a, 0x16, 0x24, 0x34, 0xe1, 0x25, 0xf1, 0x17, 0x18, 0x19, 0x1a, 0x26,
     0x27, 0x28, 0x29, 0x2a, 0x35, 0x36, 0x37, 0x38, 0x39, 0x3a, 0x43, 0x44, 0x45, 0x46, 0x47, 0x48, 0x49, 0x4a, 0x53, 0x54, 0x55, 0x56, 0x57, 0x58,
     0x59, 0x5a, 0x63, 0x64, 0x65, 0x66, 0x67, 0x68, 0x69, 0x6a, 0x73, 0x74, 0x75, 0x76, 0x77, 0x78, 0x79, 0x7a, 0x82, 0x83, 0x84, 0x85, 0x86, 0x87,
     0x88, 0x89, 0x8a, 0x92, 0x93, 0x94, 0x95, 0x96, 0x97, 0x98, 0x99, 0x9a, 0xa2, 0xa3, 0xa4, 0xa5, 0xa6, 0xa7, 0xa8, 0xa9, 0xaa, 0xb2, 0xb3, 0xb4,
     0xb5, 0xb6, 0xb7, 0xb8, 0xb9, 0xba, 0xc2, 0xc3, 0xc4, 0xc5, 0xc6, 0xc7, 0xc8, 0xc9, 0xca, 0xd2, 0xd3, 0xd4, 0xd5, 0xd6, 0xd7, 0xd8, 0xd9, 0xda,
     0xe2, 0xe3, 0xe4, 0xe5, 0xe6, 0xe7, 0xe8, 0xe9, 0xea, 0xf2, 0xf3, 0xf4, 0xf5, 0xf6, 0xf7, 0xf8, 0xf9, 0xfa}};

std::vector<std::uint8_t> jpeg_with_dht(std::vector<std::uint8_t> f) {
    if (f.size() < 4 || f[0] != 0xFF || f[1] != 0xD8) return f;
    std::size_t i = 2;
    while (i + 4 <= f.size()) {
        if (f[i] != 0xFF) return f;
        std::uint8_t m = f[i + 1];
        if (m == 0xFF) { ++i; continue; }                 // fill byte
        if (m == 0xC4) return f;                          // has its own tables
        if (m == 0xDA) {
            std::vector<std::uint8_t> seg = {0xFF, 0xC4, 0, 0};
            for (int t = 0; t < 2; ++t) {                 // DC 0/1 (values 0..11), AC 0/1
                seg.push_back(std::uint8_t(0x00 | t)); seg.insert(seg.end(), k_dc_bits[t], k_dc_bits[t] + 16); for (std::uint8_t v = 0; v < 12; ++v) seg.push_back(v);
                seg.push_back(std::uint8_t(0x10 | t)); seg.insert(seg.end(), k_ac_bits[t], k_ac_bits[t] + 16); seg.insert(seg.end(), k_ac_vals[t], k_ac_vals[t] + 162);
            }
            std::size_t len = seg.size() - 2; seg[2] = std::uint8_t(len >> 8); seg[3] = std::uint8_t(len);
            f.insert(f.begin() + std::ptrdiff_t(i), seg.begin(), seg.end());
            return f;
        }
        if (m == 0x01 || (m >= 0xD0 && m <= 0xD7)) { i += 2; continue; }   // standalone markers
        std::size_t len = std::size_t(f[i + 2]) << 8 | f[i + 3]; if (len < 2) return f;
        i += 2 + len;
    }
    return f;
}

std::vector<iio_channel> iio_channels(std::vector<std::string> attrs) {
    struct def { const char* type; sensor_kind k; int axes; double mul; };
    static const def defs[] = {
        {"accel", sensor_kind::accelerometer, 3, 1.0}, {"anglvel", sensor_kind::gyroscope, 3, 1.0},
        {"magn", sensor_kind::magnetometer, 3, 100.0 /* gauss -> uT */}, {"gravity", sensor_kind::gravity, 3, 1.0},
        {"illuminance", sensor_kind::light, 1, 1.0}, {"proximity", sensor_kind::proximity, 1, 1.0},
        {"pressure", sensor_kind::pressure, 1, 10.0 /* kPa -> hPa */}, {"temp", sensor_kind::temperature, 1, 0.001 /* milli degC */},
        {"humidityrelative", sensor_kind::humidity, 1, 0.001 /* milli %RH */}, {"steps", sensor_kind::step_counter, 1, 1.0},
        {"rot", sensor_kind::rotation_vector, 4, 1.0}};
    struct core { std::string type, index, mod; };
    std::vector<core> cores;
    std::sort(attrs.begin(), attrs.end());
    for (auto& a : attrs) {
        std::string_view s = a;
        if (s.substr(0, 3) != "in_") continue;
        if (s.size() > 4 && s.substr(s.size() - 4) == "_raw") s.remove_suffix(4);
        else if (s.size() > 6 && s.substr(s.size() - 6) == "_input") s.remove_suffix(6);
        else continue;
        s.remove_prefix(3);
        std::size_t i = 0; while (i < s.size() && s[i] >= 'a' && s[i] <= 'z') ++i;
        std::size_t j = i; while (j < s.size() && s[j] >= '0' && s[j] <= '9') ++j;
        if (i == 0 || (j < s.size() && s[j] != '_')) continue;
        cores.push_back({std::string(s.substr(0, i)), std::string(s.substr(i, j - i)), j < s.size() ? std::string(s.substr(j + 1)) : std::string()});
    }
    // Unindexed channels first, then by index number (in_temp before in_temp0 before in_temp10).
    auto idx = [](const core& c) { return c.index.empty() ? -1L : c.index.size() > 9 ? 999999999L : std::strtol(c.index.c_str(), nullptr, 10); };
    std::stable_sort(cores.begin(), cores.end(), [&](const core& a, const core& b) { return idx(a) < idx(b); });
    auto has = [&](const std::string& t, const std::string& idx, const char* mod) {
        return std::any_of(cores.begin(), cores.end(), [&](const core& c) { return c.type == t && c.index == idx && c.mod == mod; });
    };
    std::vector<iio_channel> out;
    for (auto& d : defs) {
        std::optional<iio_channel> pick;
        for (auto& c : cores) {
            if (c.type != d.type) continue;
            std::string stem = "in_" + c.type + c.index;
            if (d.axes == 3) { if (c.mod == "x" && has(c.type, c.index, "y") && has(c.type, c.index, "z")) { pick = iio_channel{d.k, stem, "in_" + c.type, 3, d.mul}; break; } }
            else if (d.axes == 4) { if (c.mod == "quaternion") { pick = iio_channel{d.k, stem + "_quaternion", "in_" + c.type, 4, d.mul}; break; } }
            else if (c.mod.empty() || (d.k == sensor_kind::temperature && c.mod == "ambient")) {
                iio_channel ch{d.k, c.mod.empty() ? stem : stem + "_" + c.mod, "in_" + c.type, 1, d.mul};
                if (d.k == sensor_kind::temperature && c.mod == "ambient") { pick = ch; break; }   // ambient beats die/object temperature
                if (!pick) pick = ch;
            }
        }
        if (pick) out.push_back(*pick);
    }
    return out;
}

xset_saver parse_xset_q(std::string_view t) {
    xset_saver r;
    auto num_after = [&](std::string_view key) {
        auto p = t.find(key); if (p == std::string_view::npos) return -1;
        p += key.size(); while (p < t.size() && t[p] == ' ') ++p;
        int v = 0; bool any = false; while (p < t.size() && t[p] >= '0' && t[p] <= '9') { v = v * 10 + (t[p++] - '0'); any = true; if (v > 1000000) return -1; }
        return any ? v : -1;
    };
    r.timeout = num_after("timeout:"); r.cycle = num_after("cycle:");
    if (t.find("DPMS is Enabled") != std::string_view::npos) r.dpms = 1;
    else if (t.find("DPMS is Disabled") != std::string_view::npos) r.dpms = 0;
    return r;
}
} // namespace detail

} // namespace forge::os::mobile
