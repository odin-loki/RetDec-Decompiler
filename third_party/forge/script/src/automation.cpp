// forge::script automation: job pipelines (forge::pipeline), file-watch triggers (forge::watch) and schedules
// (forge::every / forge::at / forge::cron and forge::scheduler). All of it runs on the calling interpreter
// thread except pipeline jobs run with parallel > 1, which are tasks (deep-copied like std::async).
// Waiting is done in short slices that call Interp::step(), so script timeouts and deadlines still apply.
#include "interp.hpp"
#include <forge/hash/blake3.hpp>
#include <forge/os/file_watch_tree.hpp>
#include <forge/os/fs.hpp>
#include <forge/os/time.hpp>
#include <algorithm>
#include <ctime>
#include <set>

namespace forge::script {
namespace {
using Args = std::vector<Value>;
using Fn = Native::fn_t;
Value strv(std::string s) { return obj(make<String>(std::move(s))); }
Value nat(const std::string& name, int lo, int hi, Fn f) { auto n = make<Native>(name, std::move(f), lo, hi); n->frozen = true; return obj(n); }
std::string s_of(Interp& in, const Value& v, const char* what) { auto* s = as<String>(v); if (!s) in.raise("TypeError", std::string(what) + " expects a string, got " + Interp::type_of(v)); return s->s; }
std::int64_t i_of(Interp& in, const Value& v, const char* what) {
    if (auto* i = std::get_if<std::int64_t>(&v)) return *i;
    if (auto* d = std::get_if<double>(&v)) return std::int64_t(*d);
    in.raise("TypeError", std::string(what) + " expects a number, got " + Interp::type_of(v)); return 0;
}
const Value* kv(const Value& bag, const std::string& k) {
    const Value* r = nullptr;
    if (auto* m = as<Map>(bag)) r = m->find_str(k);
    else if (auto* l = as<List>(bag)) for (auto& e : l->items) if (auto* p = as<List>(e); p && p->items.size() == 2) if (auto* s = as<String>(p->items[0]); s && s->s == k) r = &p->items[1];
    return r && !is_nil(*r) ? r : nullptr;
}
const Value* opt(const Args& a, std::size_t i, const std::string& k) { return i < a.size() ? kv(a[i], k) : nullptr; }
std::vector<std::string> strs(Interp& in, const Value& v) {
    std::vector<std::string> o;
    if (auto* l = as<List>(v)) for (auto& x : l->items) o.push_back(in.str(x));
    else if (auto* s = as<String>(v)) o.push_back(s->s);
    return o;
}
Value list_strs(const std::vector<std::string>& v) { auto l = make<List>(); for (auto& s : v) l->push(strv(s)); return obj(l); }
Handle& h_of(Interp& in, const Value& v, const std::string& kind) {
    auto* h = as<Handle>(v);
    if (!h || h->kind != kind) in.raise("TypeError", "expected a " + kind + " handle");
    if (h->disposed) in.raise("ObjectDisposedError", kind + " handle has been disposed");
    return *h;
}
std::pair<std::string, std::string> error_of(Interp& in, std::exception_ptr ep) {
    auto e = in.from_cpp(ep);
    if (auto* i = as<Instance>(e.v)) { auto it = i->fields.find("message"); return {i->cls->name, it != i->fields.end() ? in.str(it->second) : e.msg}; }
    return {"Error", e.msg};
}
bool fatal_type(const std::string& t) { return t == "TimeoutError" || t == "OutOfMemoryError" || t == "SystemExit"; }

// Sleep in slices, keeping the interpreter responsive to its deadline / step limits.
void nap(Interp& in, std::uint64_t ms) {
    auto end = os::monotonic_ns() + ms * 1000000ull;
    for (;;) { in.step(); auto now = os::monotonic_ns(); if (now >= end) return; os::sleep_ns(std::min<std::uint64_t>(end - now, 50'000'000ull)); }
}

// "1h30m", "10m", "2s", "500ms", "1d", "1.5s" or a number of milliseconds.
std::uint64_t duration_ms(Interp& in, const Value& v, const char* what) {
    if (std::holds_alternative<std::int64_t>(v) || std::holds_alternative<double>(v)) { auto n = i_of(in, v, what); if (n < 0) in.raise("ValueError", std::string(what) + ": negative duration"); return std::uint64_t(n); }
    auto s = s_of(in, v, what); double total = 0; std::size_t i = 0; bool any = false;
    while (i < s.size()) {
        std::size_t j = i; while (j < s.size() && (std::isdigit(static_cast<unsigned char>(s[j])) || s[j] == '.')) ++j;
        if (j == i) in.raise("ValueError", std::string(what) + ": bad duration '" + s + "'");
        double n = std::strtod(s.substr(i, j - i).c_str(), nullptr); std::size_t k = j; while (k < s.size() && std::isalpha(static_cast<unsigned char>(s[k]))) ++k;
        auto u = s.substr(j, k - j); double mul = 0;
        if (u == "ms") mul = 1; else if (u == "s" || u.empty()) mul = 1000; else if (u == "m" || u == "min") mul = 60000; else if (u == "h") mul = 3600000; else if (u == "d") mul = 86400000; else if (u == "w") mul = 7 * 86400000.0;
        else in.raise("ValueError", std::string(what) + ": unknown unit '" + u + "' in '" + s + "'");
        total += n * mul; i = k; any = true;
    }
    if (!any) in.raise("ValueError", std::string(what) + ": empty duration");
    return std::uint64_t(total);
}

// ------------------------------------------------------------------ cron
struct Cron { std::vector<bool> min, hour, dom, mon, dow; bool dom_star = true, dow_star = true; };
bool parse_cron_field(const std::string& f, int lo, int hi, std::vector<bool>& out, bool& star) {
    out.assign(std::size_t(hi + 1), false); star = f == "*" || f == "?";
    std::size_t p = 0;
    while (p <= f.size()) {
        auto c = f.find(',', p); auto part = f.substr(p, c == std::string::npos ? std::string::npos : c - p);
        int step = 1; auto sl = part.find('/');
        if (sl != std::string::npos) { step = std::atoi(part.c_str() + sl + 1); if (step <= 0) return false; part.resize(sl); }
        int a = lo, b = hi;
        if (part != "*" && part != "?") {
            auto d = part.find('-');
            if (part.empty() || !std::isdigit(static_cast<unsigned char>(part[0]))) return false;
            a = std::atoi(part.c_str()); b = d == std::string::npos ? (sl != std::string::npos ? hi : a) : std::atoi(part.c_str() + d + 1);
        }
        if (a < lo || b > hi || a > b) return false;
        for (int x = a; x <= b; x += step) out[std::size_t(x)] = true;
        if (c == std::string::npos) break; p = c + 1;
    }
    return true;
}
Cron parse_cron(Interp& in, const std::string& e) {
    static const std::pair<const char*, const char*> alias[] = {{"@hourly", "0 * * * *"}, {"@daily", "0 0 * * *"}, {"@midnight", "0 0 * * *"}, {"@weekly", "0 0 * * 0"}, {"@monthly", "0 0 1 * *"}, {"@yearly", "0 0 1 1 *"}};
    std::string s = e; for (auto& [k, v] : alias) if (s == k) s = v;
    std::vector<std::string> f; std::size_t i = 0;
    while (i < s.size()) { while (i < s.size() && s[i] == ' ') ++i; std::size_t j = i; while (j < s.size() && s[j] != ' ') ++j; if (j > i) f.push_back(s.substr(i, j - i)); i = j; }
    Cron c; bool st = false;
    if (f.size() != 5 || !parse_cron_field(f[0], 0, 59, c.min, st) || !parse_cron_field(f[1], 0, 23, c.hour, st) || !parse_cron_field(f[2], 1, 31, c.dom, c.dom_star) ||
        !parse_cron_field(f[3], 1, 12, c.mon, st) || !parse_cron_field(f[4], 0, 7, c.dow, c.dow_star))
        in.raise("ValueError", "cron: bad expression '" + e + "' (want 'min hour day-of-month month day-of-week')");
    if (c.dow[7]) c.dow[0] = true;
    return c;
}
// Next local-time minute strictly after `after_ns` that matches (Unix ns), or -1.
std::int64_t cron_next(const Cron& c, std::int64_t after_ns) {
    std::int64_t t = (after_ns / 60000000000LL + 1) * 60000000000LL;
    for (int i = 0; i < 366 * 24 * 60; ++i, t += 60000000000LL) {
        auto lt = os::local_time(t);
        if (!c.mon[std::size_t(lt.month)] || !c.hour[std::size_t(lt.hour)] || !c.min[std::size_t(lt.minute)]) continue;
        bool d1 = c.dom[std::size_t(lt.day)], d2 = c.dow[std::size_t(lt.weekday)];
        bool day = c.dom_star && c.dow_star ? true : c.dom_star ? d2 : c.dow_star ? d1 : (d1 || d2);   // POSIX: either when both set
        if (day) return t;
    }
    return -1;
}
// Next local "HH:MM[:SS]" after now.
std::int64_t at_next(Interp& in, const std::string& hhmm, std::int64_t after_ns) {
    int h = -1, m = -1, s = 0;
    if (std::sscanf(hhmm.c_str(), "%d:%d:%d", &h, &m, &s) < 2 || h < 0 || h > 23 || m < 0 || m > 59 || s < 0 || s > 59) in.raise("ValueError", "at: bad time '" + hhmm + "' (want HH:MM or HH:MM:SS)");
    auto lt = os::local_time(after_ns);
    for (int d = 0; d < 3; ++d) {
        os::civil_time c = lt; c.hour = h; c.minute = m; c.second = s; c.nanos = 0;
        auto base = os::unix_ns_from_civil(c) + std::int64_t(d) * 86400LL * 1000000000LL;
        // Re-derive the offset at the target instant (DST changes between now and then).
        c.offset_s = os::utc_offset_seconds(base / 1000000000); auto t = os::unix_ns_from_civil(c) + std::int64_t(d) * 86400LL * 1000000000LL;
        if (t > after_ns) return t;
    }
    return after_ns + 86400LL * 1000000000LL;
}

// ------------------------------------------------------------------ scheduler
struct Entry { std::string name, kind, spec, key; Value fn; std::uint64_t period_ms = 0; Cron cron; std::int64_t next = 0; std::int64_t fired = 0, limit = -1; bool active = true; };
struct Sched { std::vector<Entry> e; };
std::int64_t plan(Interp& in, Entry& x, std::int64_t now) {
    if (x.kind == "every") return now + std::int64_t(x.period_ms) * 1000000LL;
    if (x.kind == "at") return at_next(in, x.spec, now);
    return cron_next(x.cron, now);
}
// Runs due entries until the stop condition; returns the number of callbacks fired.
std::int64_t run_sched(Interp& in, Sched& s, std::int64_t max_fires, std::int64_t until_ns, Handle* h = nullptr) {
    std::int64_t fires = 0;
    for (;;) {
        Entry* nx = nullptr; for (auto& x : s.e) if (x.active && x.next > 0 && (!nx || x.next < nx->next)) nx = &x;
        if (!nx) return fires;
        if (until_ns && nx->next > until_ns) { auto now = os::unix_time_ns(); if (until_ns > now) nap(in, std::uint64_t((until_ns - now) / 1000000)); return fires; }
        auto now = os::unix_time_ns();
        if (nx->next > now) { nap(in, std::uint64_t((nx->next - now + 999999) / 1000000)); continue; }
        std::size_t idx = std::size_t(nx - s.e.data());
        Value fn = h ? h->props[nx->key] : nx->fn; Args a{std::int64_t(nx->fired)};
        auto* f = as<Function>(fn); if (f && f->decl->params.empty()) a.clear();
        Value r = in.call(fn, a);
        auto& x = s.e[idx]; ++x.fired; ++fires;
        if ((std::holds_alternative<bool>(r) && !std::get<bool>(r)) || (x.limit >= 0 && x.fired >= x.limit)) x.active = false;
        else x.next = plan(in, x, std::max(os::unix_time_ns(), x.next));
        if (max_fires >= 0 && fires >= max_fires) return fires;
    }
}
Value add_entry(Interp& in, Sched& s, const std::string& kind, Args& a, std::size_t fn_i, Handle* h = nullptr) {
    Entry x; x.kind = kind;
    if (h) { x.key = "fn:" + std::to_string(s.e.size()); h->props[x.key] = a[fn_i]; } else x.fn = a[fn_i];
    if (kind == "every") { x.period_ms = std::max<std::uint64_t>(1, duration_ms(in, a[0], "every")); x.spec = std::holds_alternative<std::int64_t>(a[0]) ? std::to_string(std::get<std::int64_t>(a[0])) + "ms" : in.str(a[0]); }
    else if (kind == "at") { x.spec = s_of(in, a[0], "at"); at_next(in, x.spec, os::unix_time_ns()); }
    else { x.spec = s_of(in, a[0], "cron"); x.cron = parse_cron(in, x.spec); }
    if (auto* v = opt(a, fn_i + 1, "times")) x.limit = i_of(in, *v, "times");
    if (auto* v = opt(a, fn_i + 1, "name")) x.name = in.str(*v); else x.name = kind + " " + x.spec;
    auto now = os::unix_time_ns();
    x.next = (kind == "every" && opt(a, fn_i + 1, "now") && Interp::truthy(*opt(a, fn_i + 1, "now"))) ? now : plan(in, x, now);
    if (x.next < 0) in.raise("ValueError", "cron: '" + x.spec + "' never matches");
    s.e.push_back(std::move(x));
    return std::int64_t(s.e.size() - 1);
}
std::pair<std::int64_t, std::int64_t> run_limits(Interp& in, const Args& a, std::size_t i) {
    std::int64_t mx = -1, until = 0;
    if (auto* v = opt(a, i, "count")) mx = i_of(in, *v, "count");
    if (auto* v = opt(a, i, "for")) until = os::unix_time_ns() + std::int64_t(duration_ms(in, *v, "for")) * 1000000LL;
    return {mx, until};
}

// ------------------------------------------------------------------ pipeline
struct Job { std::string name; std::vector<std::string> deps, inputs, outputs; std::string key; int retries = 0; };
struct Pipeline { std::vector<Job> jobs; std::string cache_path; };
const Job* find_job(const Pipeline& p, const std::string& n) { for (auto& j : p.jobs) if (j.name == n) return &j; return nullptr; }
std::vector<std::string> topo(Interp& in, const Pipeline& p, const std::vector<std::string>& targets) {
    std::vector<std::string> order; std::map<std::string, int> mark;   // 1 visiting, 2 done
    std::function<void(const std::string&, const std::string&)> visit = [&](const std::string& n, const std::string& from) {
        auto* j = find_job(p, n); if (!j) in.raise("PipelineError", "unknown job '" + n + "'" + (from.empty() ? "" : " (needed by '" + from + "')"));
        if (mark[n] == 2) return;
        if (mark[n] == 1) in.raise("PipelineError", "dependency cycle through '" + n + "'");
        mark[n] = 1; for (auto& d : j->deps) visit(d, n); mark[n] = 2; order.push_back(n);
    };
    if (targets.empty()) for (auto& j : p.jobs) visit(j.name, ""); else for (auto& t : targets) visit(t, "");
    return order;
}
std::vector<std::string> expand(const std::vector<std::string>& pats) {
    std::vector<std::string> o;
    for (auto& g : pats) {
        if (g.find_first_of("*?[") == std::string::npos) { o.push_back(g); continue; }
        if (auto r = os::fs::glob(g, ".")) for (auto& x : r.value) o.push_back(x);
    }
    std::sort(o.begin(), o.end()); o.erase(std::unique(o.begin(), o.end()), o.end()); return o;
}
// Fingerprint of the inputs (path, size, mtime) for the cache; "" when the job has no inputs.
std::string fingerprint(const Job& j) {
    if (j.inputs.empty()) return "";
    std::string acc; for (auto& f : expand(j.inputs)) { auto st = os::stat(f); acc += f + "|" + std::to_string(st.size) + "|" + std::to_string(st.mtime_ns) + "\n"; }
    for (auto& o : j.outputs) acc += "out:" + o + "|" + (os::stat(o).exists ? "1" : "0") + "\n";
    return hash::blake3::hex(hash::blake3::of(std::span<const std::uint8_t>(reinterpret_cast<const std::uint8_t*>(acc.data()), acc.size())));
}
// Make-style: every output exists and is no older than the newest input.
bool outputs_fresh(const Job& j) {
    if (j.outputs.empty()) return false;
    std::int64_t newest_in = 0, oldest_out = INT64_MAX;
    for (auto& f : expand(j.inputs)) { auto st = os::stat(f); if (!st.exists) return false; newest_in = std::max(newest_in, st.mtime_ns); }
    for (auto& o : j.outputs) { auto st = os::stat(o); if (!st.exists) return false; oldest_out = std::min(oldest_out, st.mtime_ns); }
    return j.inputs.empty() || newest_in <= oldest_out;
}

void install(Shared& sh) {
    auto& B = sh.builtins; auto& M = sh.methods;

    // ---- pipelines
    B["forge::pipeline"] = nat("forge::pipeline", 0, 1, [](Interp& in, Args& a) -> Value {
        auto p = std::make_shared<Pipeline>();
        if (auto* v = opt(a, 0, "cache")) p->cache_path = in.str(*v);
        auto h = make<Handle>("pipeline"); h->payload = p; return obj(h);
    });
    M["handle:pipeline"]["job"] = nat("pipeline.job", 3, 5, [](Interp& in, Args& a) -> Value {
        // job(name, fn) | job(name, deps, fn) | job(name, deps, fn, opts)
        auto& h = h_of(in, a[0], "pipeline"); auto p = h.payload_as<Pipeline>();
        Job j; j.name = s_of(in, a[1], "pipeline.job name");
        std::size_t fi = 2;
        if (a.size() > 3 && (as<List>(a[2]) || as<String>(a[2]))) { j.deps = strs(in, a[2]); fi = 3; }
        j.key = "job:" + j.name; h.props[j.key] = a[fi];
        if (auto* v = opt(a, fi + 1, "inputs")) j.inputs = strs(in, *v);
        if (auto* v = opt(a, fi + 1, "outputs")) j.outputs = strs(in, *v);
        if (auto* v = opt(a, fi + 1, "retries")) j.retries = int(i_of(in, *v, "retries"));
        if (find_job(*p, j.name)) in.raise("PipelineError", "duplicate job '" + j.name + "'");
        p->jobs.push_back(std::move(j));
        return a[0];
    });
    M["handle:pipeline"]["order"] = nat("pipeline.order", 1, 2, [](Interp& in, Args& a) -> Value {
        auto p = h_of(in, a[0], "pipeline").payload_as<Pipeline>(); return list_strs(topo(in, *p, a.size() > 1 ? strs(in, a[1]) : std::vector<std::string>{}));
    });
    M["handle:pipeline"]["run"] = nat("pipeline.run", 1, 2, [](Interp& in, Args& a) -> Value {
        auto& ph = h_of(in, a[0], "pipeline"); auto p = ph.payload_as<Pipeline>();
        std::vector<std::string> targets; int par = 1; bool keep_going = false, dry = false, force = false, raise_on_fail = false;
        if (auto* v = opt(a, 1, "targets")) targets = strs(in, *v);
        if (auto* v = opt(a, 1, "parallel")) par = std::max<int>(1, int(i_of(in, *v, "parallel")));
        if (auto* v = opt(a, 1, "keep_going")) keep_going = Interp::truthy(*v);
        if (auto* v = opt(a, 1, "dry_run")) dry = Interp::truthy(*v);
        if (auto* v = opt(a, 1, "force")) force = Interp::truthy(*v);
        if (auto* v = opt(a, 1, "raise")) raise_on_fail = Interp::truthy(*v);
        auto order = topo(in, *p, targets);
        // Cache: job -> fingerprint of the last successful run.
        std::map<std::string, std::string> cache;
        if (!p->cache_path.empty()) if (auto t = os::read_file(p->cache_path)) { std::size_t i = 0; auto& tx = t.value; while (i < tx.size()) { auto nl = tx.find('\n', i); if (nl == std::string::npos) nl = tx.size(); auto ln = tx.substr(i, nl - i); auto tb = ln.rfind('\t'); if (tb != std::string::npos) cache[ln.substr(0, tb)] = ln.substr(tb + 1); i = nl + 1; } }
        std::map<std::string, std::string> state;   // pending / ok / skipped / failed / cancelled
        auto jobs_m = make<Map>(); bool any_failed = false;
        auto record = [&](const std::string& n, const std::string& st, double ms, const std::string& et, const std::string& em, Value v) {
            state[n] = st; auto m = make<Map>(); m->set_str("status", strv(st)); m->set_str("ms", ms);
            if (!et.empty()) { m->set_str("error_type", strv(et)); m->set_str("error", strv(em)); }
            if (!is_nil(v)) m->set_str("value", v);
            jobs_m->set_str(n, obj(m));
        };
        for (auto& n : order) state[n] = "pending";
        std::set<std::string> fps_dirty;
        while (true) {
            std::vector<const Job*> ready;
            for (auto& n : order) {
                if (state[n] != "pending") continue;
                auto* j = find_job(*p, n); bool blocked = false, cancel = false;
                for (auto& d : j->deps) { auto& s = state[d]; if (s == "pending") blocked = true; else if (s == "failed" || s == "cancelled") cancel = true; }
                if (cancel) { record(n, "cancelled", 0, "", "", Nil{}); continue; }
                if (!blocked) ready.push_back(j);
            }
            if (ready.empty()) break;
            if (any_failed && !keep_going) { for (auto* j : ready) record(j->name, "cancelled", 0, "", "", Nil{}); continue; }
            // Up-to-date checks first (cheap, on this thread).
            std::vector<const Job*> run;
            for (auto* j : ready) {
                bool deps_ran = false; for (auto& d : j->deps) deps_ran |= state[d] == "ok";
                std::string fp = fingerprint(*j);
                if (!force && !deps_ran && (outputs_fresh(*j) || (!fp.empty() && cache.count(j->name) && cache[j->name] == fp))) { record(j->name, "skipped", 0, "", "", Nil{}); continue; }
                if (dry) { record(j->name, "would_run", 0, "", "", Nil{}); continue; }
                run.push_back(j);
            }
            for (std::size_t b = 0; b < run.size(); b += std::size_t(par)) {
                std::size_t e = std::min(run.size(), b + std::size_t(par));
                if (any_failed && !keep_going) { for (std::size_t i = b; i < run.size(); ++i) record(run[i]->name, "cancelled", 0, "", "", Nil{}); break; }
                auto exec = [&](const Job* j, auto&& once) {
                    auto t0 = os::monotonic_ns(); std::string et, em; Value v;
                    for (int attempt = 0; attempt <= j->retries; ++attempt) {
                        et.clear(); em.clear();
                        try { v = once(j); break; }
                        catch (...) { auto [t, m] = error_of(in, std::current_exception()); if (fatal_type(t)) throw; et = t; em = m; }
                    }
                    double ms = double(os::monotonic_ns() - t0) / 1e6;
                    if (!et.empty()) { any_failed = true; record(j->name, "failed", ms, et, em, Nil{}); }
                    else { record(j->name, "ok", ms, "", "", v); fps_dirty.insert(j->name); }
                };
                if (e - b == 1 || par == 1) {
                    for (std::size_t i = b; i < e; ++i) { if (any_failed && !keep_going) { record(run[i]->name, "cancelled", 0, "", "", Nil{}); continue; } exec(run[i], [&](const Job* j) { Args x; return in.call(ph.props[j->key], x); }); }
                } else {
                    std::vector<Value> tasks; for (std::size_t i = b; i < e; ++i) tasks.push_back(in.spawn(ph.props[run[i]->key], {}));
                    for (std::size_t i = b; i < e; ++i) {
                        bool first = true; Value t = tasks[i - b];
                        exec(run[i], [&](const Job* j) { if (!first) t = in.spawn(ph.props[j->key], {}); first = false; return in.await(t); });
                    }
                }
            }
        }
        if (!p->cache_path.empty() && !dry) {
            for (auto& n : fps_dirty) { auto fp = fingerprint(*find_job(*p, n)); if (fp.empty()) cache.erase(n); else cache[n] = fp; }
            for (auto& [n, s] : state) if (s == "failed") cache.erase(n);
            std::string o; for (auto& [k, v] : cache) o += k + "\t" + v + "\n";
            os::fs::write_file_atomic(p->cache_path, o);
        }
        auto r = make<Map>(); r->set_str("ok", !any_failed); r->set_str("jobs", obj(jobs_m)); r->set_str("order", list_strs(order));
        if (any_failed && raise_on_fail) {
            std::string msg; for (auto& [n, s] : state) if (s == "failed") msg += (msg.empty() ? "" : ", ") + n;
            in.raise("PipelineError", "failed jobs: " + msg);
        }
        return obj(r);
    });

    // ---- file-watch triggers: forge::watch(pattern(s), fn(changes), opts) blocks and calls fn per debounced batch.
    B["forge::watch"] = nat("forge::watch", 2, 3, [](Interp& in, Args& a) -> Value {
        if (!in.caps.fs) in.raise("PermissionError", "file access is disabled in this sandbox");
        auto pats = strs(in, a[0]); if (pats.empty()) in.raise("ValueError", "forge::watch: no pattern");
        std::string root = ".";
        if (auto* v = opt(a, 2, "root")) root = in.str(*v);
        else {   // the literal directory prefix of the first pattern
            auto& g = pats[0]; std::size_t cut = 0;
            for (std::size_t i = 0; i < g.size(); ++i) { if (g[i] == '*' || g[i] == '?' || g[i] == '[') break; if (g[i] == '/') cut = i; }
            if (g.find_first_of("*?[") == std::string::npos) { auto st = os::stat(g); root = st.is_dir ? g : (cut ? g.substr(0, cut) : "."); if (!st.is_dir) pats[0] = g.substr(cut ? cut + 1 : 0); else pats[0] = "**"; }
            else if (cut) { root = g.substr(0, cut); for (auto& p : pats) if (p.compare(0, cut + 1, g.substr(0, cut + 1)) == 0) p = p.substr(cut + 1); }
        }
        enforce_scope(in, SecScope::fs_read, root);
        os::tree_watch_opts o; o.debounce_ms = 100;
        if (auto* v = opt(a, 2, "debounce_ms")) o.debounce_ms = int(i_of(in, *v, "debounce_ms"));
        if (auto* v = opt(a, 2, "polling")) o.force_polling = Interp::truthy(*v);
        if (auto* v = opt(a, 2, "ignore")) o.ignore = strs(in, *v);
        std::int64_t max_batches = -1, until = 0;
        if (auto* v = opt(a, 2, "count")) max_batches = i_of(in, *v, "count");
        if (auto* v = opt(a, 2, "for")) until = std::int64_t(os::monotonic_ns()) + std::int64_t(duration_ms(in, *v, "for")) * 1000000LL;
        auto w = os::tree_watcher::open(root, o); if (!w) in.raise("IOError", w.err.what);
        if (auto* v = opt(a, 2, "ready")) { Args x; in.call(*v, x); }   // hook: runs once the watch is armed
        std::string rootn = root; while (rootn.size() > 1 && rootn.back() == '/') rootn.pop_back();
        std::int64_t batches = 0;
        while (max_batches < 0 || batches < max_batches) {
            int slice = 100;
            if (until) { auto now = std::int64_t(os::monotonic_ns()); if (now >= until) break; slice = int(std::min<std::int64_t>(100, (until - now) / 1000000 + 1)); }
            in.step();
            auto ev = (*w)->poll(slice);
            auto l = make<List>();
            for (auto& e : ev) {
                auto rel = e.path.size() > rootn.size() && e.path.compare(0, rootn.size(), rootn) == 0 ? e.path.substr(rootn.size() + 1) : e.path;
                bool hit = false; for (auto& p : pats) hit |= os::fs::glob_match(p, rel);
                if (!hit) continue;
                auto m = make<Map>(); m->set_str("path", strv(e.path)); m->set_str("kind", strv(os::watch_kind_name(e.kind))); l->push(obj(m));
            }
            if (l->items.empty()) continue;
            ++batches;
            Args x{obj(l)}; Value r = in.call(a[1], x);
            if (std::holds_alternative<bool>(r) && !std::get<bool>(r)) break;
        }
        return batches;
    });

    // ---- schedules
    auto one_shot = [](const std::string& kind) {
        return [kind](Interp& in, Args& a) -> Value {
            Sched s; add_entry(in, s, kind, a, 1);
            auto [mx, until] = run_limits(in, a, 2);
            return run_sched(in, s, mx, until);
        };
    };
    B["forge::every"] = nat("forge::every", 2, 3, one_shot("every"));
    B["forge::at"] = nat("forge::at", 2, 3, one_shot("at"));
    B["forge::cron"] = nat("forge::cron", 2, 3, one_shot("cron"));
    B["forge::cron_next"] = nat("forge::cron_next", 1, 2, [](Interp& in, Args& a) -> Value {
        auto c = parse_cron(in, s_of(in, a[0], "cron_next"));
        std::int64_t from = a.size() > 1 ? std::int64_t(std::get_if<double>(&a[1]) ? std::get<double>(a[1]) * 1e9 : double(i_of(in, a[1], "from")) * 1e9) : os::unix_time_ns();
        auto t = cron_next(c, from); if (t < 0) return Nil{};
        return double(t) / 1e9;
    });
    B["forge::duration_ms"] = nat("forge::duration_ms", 1, 1, [](Interp& in, Args& a) -> Value { return std::int64_t(duration_ms(in, a[0], "duration")); });
    B["forge::scheduler"] = nat("forge::scheduler", 0, 0, [](Interp&, Args&) -> Value { auto h = make<Handle>("scheduler"); h->payload = std::make_shared<Sched>(); return obj(h); });
    for (const char* k : {"every", "at", "cron"}) {
        M["handle:scheduler"][k] = nat(std::string("scheduler.") + k, 3, 4, [kind = std::string(k)](Interp& in, Args& a) -> Value {
            auto& h = h_of(in, a[0], "scheduler"); auto s = h.payload_as<Sched>(); Args rest(a.begin() + 1, a.end()); return add_entry(in, *s, kind, rest, 1, &h);
        });
    }
    M["handle:scheduler"]["cancel"] = nat("scheduler.cancel", 2, 2, [](Interp& in, Args& a) -> Value {
        auto s = h_of(in, a[0], "scheduler").payload_as<Sched>(); auto i = i_of(in, a[1], "id");
        if (i < 0 || std::size_t(i) >= s->e.size()) return false; bool was = s->e[std::size_t(i)].active; s->e[std::size_t(i)].active = false; return was;
    });
    M["handle:scheduler"]["upcoming"] = nat("scheduler.upcoming", 1, 1, [](Interp& in, Args& a) -> Value {
        auto s = h_of(in, a[0], "scheduler").payload_as<Sched>(); auto l = make<List>();
        std::vector<const Entry*> v; for (auto& x : s->e) if (x.active) v.push_back(&x);
        std::sort(v.begin(), v.end(), [](auto* x, auto* y) { return x->next < y->next; });
        for (auto* x : v) { auto m = make<Map>(); m->set_str("name", strv(x->name)); m->set_str("at", strv(os::format_local_iso8601(x->next, 0))); m->set_str("fired", x->fired); l->push(obj(m)); }
        return obj(l);
    });
    M["handle:scheduler"]["run"] = nat("scheduler.run", 1, 2, [](Interp& in, Args& a) -> Value {
        auto& h = h_of(in, a[0], "scheduler"); auto s = h.payload_as<Sched>(); auto [mx, until] = run_limits(in, a, 1);
        return run_sched(in, *s, mx, until, &h);
    });
}
} // namespace
void install_automation(Shared& sh) { install(sh); }
} // namespace forge::script
