// forge::script bindings for forge::os::mobile:
//   forge::mobile::device([{"sim": json} | {"sim_file": path} | {"null": true}])  -> "mobile" handle
//   forge::sensors::list([dev]) / forge::sensors::open(kind[, hz[, dev]])        -> "sensor" handle
//   forge::touch::open([dev])                                                    -> "touch" handle
// Streams are polled (poll/gestures) or pumped into a forge::channel (pump) from the interpreter or a task
// thread; every handle owns its device/stream through a shared_ptr copied per call, so disposing a handle
// while another call is running is safe (borrow-safe: no raw pointers outlive the call).
// Device access is gated by the `env` capability ("device access").
#include "interp.hpp"
#include "objects.hpp"
#include <forge/os/mobile.hpp>

namespace forge::script {
namespace {
namespace mob = forge::os::mobile;
using Args = std::vector<Value>;
using Fn = Native::fn_t;
Value strv(std::string s) { return obj(make<String>(std::move(s))); }
Value nat(const std::string& name, int lo, int hi, Fn f) { auto n = make<Native>(name, std::move(f), lo, hi); n->frozen = true; return obj(n); }
std::string s_of(Interp& in, const Value& v, const char* what) { auto* s = as<String>(v); if (!s) in.raise("TypeError", std::string(what) + " expects a string, got " + Interp::type_of(v)); return s->s; }
double f_of(Interp& in, const Value& v, const char* what) {
    if (auto* i = std::get_if<std::int64_t>(&v)) return double(*i);
    if (auto* d = std::get_if<double>(&v)) return *d;
    in.raise("TypeError", std::string(what) + " expects a number, got " + Interp::type_of(v)); return 0;
}
int i_of(Interp& in, const Value& v, const char* what) { return int(f_of(in, v, what)); }
void need_dev(Interp& in) { if (!in.caps.env) in.raise("PermissionError", "device access is disabled in this sandbox"); }
// Option bags arrive as a Map or as a list of {key, value} pairs (`{{"sim", trace}}`).
const Value* kv(const Value& bag, const std::string& k) {
    const Value* r = nullptr;
    if (auto* m = as<Map>(bag)) r = m->find_str(k);
    else if (auto* l = as<List>(bag)) for (auto& e : l->items) if (auto* p = as<List>(e); p && p->items.size() == 2) if (auto* s = as<String>(p->items[0]); s && s->s == k) r = &p->items[1];
    return r && !is_nil(*r) ? r : nullptr;
}
Handle& h_of(Interp& in, const Value& v, const std::string& kind) {
    auto* h = as<Handle>(v);
    if (!h || h->kind != kind) in.raise("TypeError", "expected a " + kind + " handle");
    if (h->disposed) in.raise("ObjectDisposedError", kind + " handle has been disposed");
    return *h;
}
template <class T> T okv(Interp& in, os::result<T> r) { if (!r) in.raise(os::is_unsupported(r.err) ? "NotSupportedError" : "IOError", r.err.what); return std::move(r.value); }
void oks(Interp& in, const os::status& s) { if (!s) in.raise(os::is_unsupported(s.err) ? "NotSupportedError" : "IOError", s.err.what); }

struct sensor_h { std::shared_ptr<mob::device> dev; mob::sensor_stream s; };
struct touch_h { std::shared_ptr<mob::device> dev; mob::touch_stream s; };

std::shared_ptr<mob::device> platform(Interp& in) {
    static std::mutex m; static std::weak_ptr<mob::device> w;
    std::lock_guard g(m); if (auto d = w.lock()) return d;
    auto d = std::make_shared<mob::device>(okv(in, mob::device::open())); w = d; return d;
}
std::shared_ptr<mob::device> dev_arg(Interp& in, const Args& a, std::size_t i) {
    need_dev(in);
    if (i < a.size() && !is_nil(a[i])) return h_of(in, a[i], "mobile").payload_as<mob::device>();
    return platform(in);
}
Value map_of(std::initializer_list<std::pair<const char*, Value>> kv) { auto m = make<Map>(); for (auto& [k, v] : kv) m->set_str(k, v); return obj(m); }
Value sensor_v(const mob::sensor_event& e) {
    auto l = make<List>(); for (int i = 0; i < e.n; ++i) l->push(e.v[std::size_t(i)]);
    return map_of({{"type", strv("sensor")}, {"kind", strv(mob::sensor_name(e.kind))}, {"t_ns", std::int64_t(e.t_ns)}, {"v", obj(l)}});
}
Value touch_v(const mob::touch_event& e) {
    return map_of({{"type", strv("touch")}, {"phase", strv(mob::touch_phase_name(e.phase))}, {"id", std::int64_t(e.id)}, {"x", e.x}, {"y", e.y},
                   {"pressure", e.pressure}, {"pointers", std::int64_t(e.pointers)}, {"t_ns", std::int64_t(e.t_ns)}});
}
Value gesture_v(const mob::gesture& g) {
    return map_of({{"type", strv("gesture")}, {"kind", strv(mob::gesture_name(g.kind))}, {"x", g.x}, {"y", g.y}, {"dx", g.dx}, {"dy", g.dy},
                   {"scale", g.scale}, {"rotation", g.rotation}, {"t_ns", std::int64_t(g.t_ns)}});
}
template <class V> Value list_of(const V& v, Value (*f)(const typename V::value_type&)) { auto l = make<List>(); for (auto& x : v) l->push(f(x)); return obj(l); }
// Push values into a channel without blocking: stops at capacity (the rest stay queued in the stream).
std::int64_t pump_into(Interp& in, const Value& ch, std::vector<Value> vals) {
    auto* c = as<Channel>(ch); if (!c) in.raise("TypeError", "pump expects a channel");
    auto st = c->st; std::lock_guard g(st->m);
    if (st->closed) in.raise("ValueError", "pump into a closed channel");
    std::int64_t n = 0; for (auto& v : vals) { st->q.push_back(std::move(v)); ++n; }
    st->cv.notify_all(); return n;
}
std::size_t room(const Value& ch, std::size_t max) {
    auto* c = as<Channel>(ch); if (!c) return max; std::lock_guard g(c->st->m);
    if (!c->st->cap) return max; return c->st->q.size() >= c->st->cap ? 0 : std::min(max, c->st->cap - c->st->q.size());
}

void install(Shared& sh) {
    auto& B = sh.builtins; auto& M = sh.methods;
    B["forge::mobile::device"] = nat("forge::mobile::device", 0, 1, [](Interp& in, Args& a) -> Value {
        need_dev(in); std::shared_ptr<mob::device> d;
        if (!a.empty()) {
            if (auto* v = kv(a[0], "sim")) d = std::make_shared<mob::device>(okv(in, mob::device::simulated(in.str(*v))));
            else if (auto* f = kv(a[0], "sim_file")) { if (!in.caps.fs) in.raise("PermissionError", "file access is disabled in this sandbox"); enforce_scope(in, SecScope::fs_read, in.str(*f)); d = std::make_shared<mob::device>(okv(in, mob::device::simulated_file(in.str(*f)))); }
            else if (auto* n = kv(a[0], "null"); n && Interp::truthy(*n)) d = std::make_shared<mob::device>(okv(in, mob::device::null_device()));
        }
        if (!d) d = platform(in);
        auto h = make<Handle>("mobile"); h->payload = d; return obj(h);
    });
    B["forge::sensors::list"] = nat("forge::sensors::list", 0, 1, [](Interp& in, Args& a) -> Value {
        auto d = dev_arg(in, a, 0); auto l = make<List>();
        for (auto& s : okv(in, d->sensors())) l->push(map_of({{"kind", strv(mob::sensor_name(s.kind))}, {"name", strv(s.name)}, {"vendor", strv(s.vendor)}, {"max_rate_hz", s.max_rate_hz}}));
        return obj(l);
    });
    B["forge::sensors::open"] = nat("forge::sensors::open", 1, 3, [](Interp& in, Args& a) -> Value {
        auto d = dev_arg(in, a, 2); auto k = mob::parse_sensor(s_of(in, a[0], "sensors::open kind"));
        if (!k) in.raise("ValueError", "unknown sensor kind '" + in.str(a[0]) + "'");
        double hz = a.size() > 1 && !is_nil(a[1]) ? f_of(in, a[1], "sensors::open rate") : 50;
        auto p = std::make_shared<sensor_h>(sensor_h{d, okv(in, d->open_sensor(*k, hz))});
        auto h = make<Handle>("sensor"); h->payload = p; h->closer = [p] { p->s = {}; }; return obj(h);
    });
    B["forge::touch::open"] = nat("forge::touch::open", 0, 1, [](Interp& in, Args& a) -> Value {
        auto d = dev_arg(in, a, 0);
        auto p = std::make_shared<touch_h>(touch_h{d, okv(in, d->open_touch())});
        auto h = make<Handle>("touch"); h->payload = p; h->closer = [p] { p->s = {}; }; return obj(h);
    });
    auto to = [](Interp& in, const Args& a, std::size_t i) { return i < a.size() && !is_nil(a[i]) ? i_of(in, a[i], "timeout") : 0; };
    auto& S = M["handle:sensor"];
    S["poll"] = nat("sensor.poll", 1, 2, [to](Interp& in, Args& a) -> Value { auto p = h_of(in, a[0], "sensor").payload_as<sensor_h>(); return list_of(p->s.poll(to(in, a, 1)), sensor_v); });
    S["pump"] = nat("sensor.pump", 2, 4, [to](Interp& in, Args& a) -> Value {
        auto p = h_of(in, a[0], "sensor").payload_as<sensor_h>(); std::size_t max = a.size() > 2 && !is_nil(a[2]) ? std::size_t(i_of(in, a[2], "max")) : 1024;
        std::vector<Value> v; for (auto& e : p->s.poll(to(in, a, 3), room(a[1], max))) v.push_back(sensor_v(e)); return pump_into(in, a[1], std::move(v));
    });
    S["kind"] = nat("sensor.kind", 1, 1, [](Interp& in, Args& a) -> Value { return strv(mob::sensor_name(h_of(in, a[0], "sensor").payload_as<sensor_h>()->s.kind())); });
    S["dropped"] = nat("sensor.dropped", 1, 1, [](Interp& in, Args& a) -> Value { return std::int64_t(h_of(in, a[0], "sensor").payload_as<sensor_h>()->s.dropped()); });
    S["finished"] = nat("sensor.finished", 1, 1, [](Interp& in, Args& a) -> Value { return h_of(in, a[0], "sensor").payload_as<sensor_h>()->s.finished(); });
    S["close"] = nat("sensor.close", 1, 1, [](Interp& in, Args& a) -> Value { in.dispose(a[0]); return Nil{}; });
    auto& T = M["handle:touch"];
    T["poll"] = nat("touch.poll", 1, 2, [to](Interp& in, Args& a) -> Value { auto p = h_of(in, a[0], "touch").payload_as<touch_h>(); return list_of(p->s.poll(to(in, a, 1)), touch_v); });
    T["gestures"] = nat("touch.gestures", 1, 2, [to](Interp& in, Args& a) -> Value { auto p = h_of(in, a[0], "touch").payload_as<touch_h>(); return list_of(p->s.gestures(to(in, a, 1)), gesture_v); });
    T["pump"] = nat("touch.pump", 2, 4, [to](Interp& in, Args& a) -> Value {   // pumps recognised gestures
        auto p = h_of(in, a[0], "touch").payload_as<touch_h>(); std::size_t max = a.size() > 2 && !is_nil(a[2]) ? std::size_t(i_of(in, a[2], "max")) : 1024;
        std::vector<Value> v; for (auto& g : p->s.gestures(to(in, a, 3))) { if (v.size() >= room(a[1], max)) break; v.push_back(gesture_v(g)); } return pump_into(in, a[1], std::move(v));
    });
    T["finished"] = nat("touch.finished", 1, 1, [](Interp& in, Args& a) -> Value { return h_of(in, a[0], "touch").payload_as<touch_h>()->s.finished(); });
    T["close"] = nat("touch.close", 1, 1, [](Interp& in, Args& a) -> Value { in.dispose(a[0]); return Nil{}; });

    auto& D = M["handle:mobile"];
    auto dv = [](Interp& in, const Value& v) { return h_of(in, v, "mobile").payload_as<mob::device>(); };
    D["backend"] = nat("mobile.backend", 1, 1, [dv](Interp& in, Args& a) -> Value { return strv(dv(in, a[0])->backend_name()); });
    D["display"] = nat("mobile.display", 1, 1, [dv](Interp& in, Args& a) -> Value {
        auto d = okv(in, dv(in, a[0])->display());
        return map_of({{"width", std::int64_t(d.width_px)}, {"height", std::int64_t(d.height_px)}, {"dpi", d.dpi}, {"scale", d.scale}, {"refresh_hz", d.refresh_hz}, {"orientation", strv(mob::orientation_name(d.orient))}});
    });
    D["power"] = nat("mobile.power", 1, 1, [dv](Interp& in, Args& a) -> Value {
        auto p = okv(in, dv(in, a[0])->power());
        return map_of({{"has_battery", p.has_battery}, {"battery_percent", std::int64_t(p.battery_percent)}, {"charging", p.charging}, {"low_power_mode", p.low_power_mode}});
    });
    D["connectivity"] = nat("mobile.connectivity", 1, 1, [dv](Interp& in, Args& a) -> Value {
        auto c = okv(in, dv(in, a[0])->connectivity());
        return map_of({{"online", c.online}, {"transport", strv(c.transport)}, {"ssid", strv(c.ssid)}, {"signal_percent", std::int64_t(c.signal_percent)}, {"metered", c.metered}});
    });
    D["info"] = nat("mobile.info", 1, 1, [dv](Interp& in, Args& a) -> Value {
        auto i = okv(in, dv(in, a[0])->info());
        return map_of({{"manufacturer", strv(i.manufacturer)}, {"model", strv(i.model)}, {"os", strv(i.os_name)}, {"os_version", strv(i.os_version)}, {"arch", strv(i.arch)}, {"form", strv(mob::form_factor_name(i.form))}});
    });
    D["vibrate"] = nat("mobile.vibrate", 2, 3, [dv](Interp& in, Args& a) -> Value { oks(in, dv(in, a[0])->vibrate(i_of(in, a[1], "ms"), a.size() > 2 ? f_of(in, a[2], "amplitude") : 1.0)); return Nil{}; });
    D["notify"] = nat("mobile.notify", 3, 3, [dv](Interp& in, Args& a) -> Value { oks(in, dv(in, a[0])->notify(in.str(a[1]), in.str(a[2]))); return Nil{}; });
    D["clipboard_get"] = nat("mobile.clipboard_get", 1, 1, [dv](Interp& in, Args& a) -> Value { return strv(okv(in, dv(in, a[0])->clipboard_get())); });
    D["clipboard_set"] = nat("mobile.clipboard_set", 2, 2, [dv](Interp& in, Args& a) -> Value { oks(in, dv(in, a[0])->clipboard_set(in.str(a[1]))); return Nil{}; });
    D["brightness"] = nat("mobile.brightness", 1, 1, [dv](Interp& in, Args& a) -> Value { return okv(in, dv(in, a[0])->brightness()); });
    D["set_brightness"] = nat("mobile.set_brightness", 2, 2, [dv](Interp& in, Args& a) -> Value { oks(in, dv(in, a[0])->set_brightness(f_of(in, a[1], "level"))); return Nil{}; });
    D["keep_awake"] = nat("mobile.keep_awake", 2, 2, [dv](Interp& in, Args& a) -> Value { oks(in, dv(in, a[0])->keep_awake(Interp::truthy(a[1]))); return Nil{}; });
    D["torch"] = nat("mobile.torch", 2, 2, [dv](Interp& in, Args& a) -> Value { oks(in, dv(in, a[0])->torch(Interp::truthy(a[1]))); return Nil{}; });
    D["open_url"] = nat("mobile.open_url", 2, 2, [dv](Interp& in, Args& a) -> Value { oks(in, dv(in, a[0])->open_url(in.str(a[1]))); return Nil{}; });
    D["permission"] = nat("mobile.permission", 2, 2, [dv](Interp& in, Args& a) -> Value {
        auto p = mob::parse_permission(in.str(a[1])); if (!p) in.raise("ValueError", "unknown permission"); return strv(mob::permission_state_name(okv(in, dv(in, a[0])->permission_status(*p))));
    });
    D["request_permission"] = nat("mobile.request_permission", 2, 2, [dv](Interp& in, Args& a) -> Value {
        auto p = mob::parse_permission(in.str(a[1])); if (!p) in.raise("ValueError", "unknown permission"); return bool(dv(in, a[0])->request_permission(*p));
    });
    D["sim_log"] = nat("mobile.sim_log", 1, 1, [dv](Interp& in, Args& a) -> Value {
        auto l = make<List>(); for (auto& r : dv(in, a[0])->sim_log()) l->push(map_of({{"what", strv(r.what)}, {"detail", strv(r.detail)}})); return obj(l);
    });
    D["close"] = nat("mobile.close", 1, 1, [](Interp& in, Args& a) -> Value { in.dispose(a[0]); return Nil{}; });
}
} // namespace
void install_mobile(Shared& sh) { install(sh); }
} // namespace forge::script
