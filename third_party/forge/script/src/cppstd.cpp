#include <cerrno>
#include <cctype>
#include <cstdlib>
#include <version>
// C++ standard-library surface for FORGE script: std::cout/println/format, containers' C++ method
// names, iterators, smart-pointer helpers, and the forge:: runtime helpers the parser lowers to.
#include "interp.hpp"
#include "shells/shell.hpp"
#include <forge/script/script.hpp>
#include <forge/os/process.hpp>
#include <algorithm>
#include <charconv>
#include <cmath>
#include <format>
#include <limits>
#include <numeric>
#include <thread>

namespace forge::script {

namespace {
using Args = std::vector<Value>;
using Fn = Native::fn_t;
Value nat(const std::string& name, int lo, int hi, Fn f) { auto n = make<Native>(name, std::move(f), lo, hi); n->frozen = true; return obj(n); }
Value sv(std::string s) { return obj(make<String>(std::move(s))); }
std::int64_t iv(Interp& in, const Value& v, const char* w) {
    if (auto* i = std::get_if<std::int64_t>(&v)) return *i;
    if (auto* b = std::get_if<bool>(&v)) return *b;
    if (auto* d = std::get_if<double>(&v)) return std::int64_t(*d);
    in.raise("TypeError", std::string(w) + " expects an int, got " + Interp::type_of(v));
}
double dv(Interp& in, const Value& v, const char* w) {
    if (auto* i = std::get_if<std::int64_t>(&v)) return double(*i);
    if (auto* d = std::get_if<double>(&v)) return *d;
    if (auto* b = std::get_if<bool>(&v)) return *b;
    in.raise("TypeError", std::string(w) + " expects a number, got " + Interp::type_of(v));
}
std::string& sref(Interp& in, const Value& v, const char* w) {
    auto* s = as<String>(v); if (!s) in.raise("TypeError", std::string(w) + " expects a string, got " + Interp::type_of(v));
    return s->s;
}
List& lref(Interp& in, const Value& v, const char* w) {
    auto* l = as<List>(v); if (!l) in.raise("TypeError", std::string(w) + " expects a std::vector, got " + Interp::type_of(v));
    return *l;
}
void mutable_(Interp& in, const Obj& o) { if (o.frozen) in.raise("TypeError", std::string("cannot modify a frozen ") + o.type_name()); }

// iostream manipulators
struct Manip final : Obj {
    std::string kind; std::int64_t n = 0;
    Manip(std::string k, std::int64_t v) : kind(std::move(k)), n(v) {}
    const char* type_name() const override { return "manipulator"; }
};

// The [lo, hi) range of a list addressed by two iterators (or a whole container).
struct Span { Value c; std::int64_t lo, hi; };
Span span(Interp& in, const Value& b, const Value& e, const char* w) {
    auto* pb = as<PosIt>(b); auto* pe = as<PosIt>(e);
    if (!pb || !pe) in.raise("TypeError", std::string(w) + " expects two iterators (v.begin(), v.end())");
    auto* ob = std::get_if<Ref<Obj>>(&pb->c); auto* oe = std::get_if<Ref<Obj>>(&pe->c);
    if (!ob || !oe || ob->get() != oe->get()) in.raise("ValueError", std::string(w) + ": iterators belong to different containers");
    return {pb->c, pb->i, std::max(pb->i, pe->i)};
}
std::int64_t clen(const Value& c) {
    if (auto* l = as<List>(c)) return std::int64_t(l->items.size());
    if (auto* m = as<Map>(c)) return std::int64_t(m->entries.size());
    if (auto* s = as<String>(c)) return std::int64_t(s->s.size());
    return 0;
}
Value elem(Interp& in, const Value& c, std::int64_t i) {
    if (i < 0 || i >= clen(c)) in.raise("IndexError", "dereferencing an iterator out of range (end() or past it)");
    if (auto* l = as<List>(c)) return l->items[std::size_t(i)];
    if (auto* m = as<Map>(c)) { auto& [k, v] = m->entries[std::size_t(i)]; return obj(make<List>(std::vector<Value>{k, v})); }
    if (auto* s = as<String>(c)) return sv(std::string(1, s->s[std::size_t(i)]));
    return Nil{};
}
Value it(const Value& c, std::int64_t i) { return obj(make<PosIt>(c, i)); }
// whole-container or iterator-pair arguments: f(v) or f(v.begin(), v.end())
Span range_args(Interp& in, Args& a, std::size_t& next, const char* w) {
    if (as<PosIt>(a[0])) { if (a.size() < 2) in.raise("TypeError", std::string(w) + " needs begin and end"); next = 2; return span(in, a[0], a[1], w); }
    next = 1; return {a[0], 0, clen(a[0])};
}
List& span_list(Interp& in, const Span& s, const char* w) { return lref(in, s.c, w); }
} // namespace

Obj* moved_sentinel() {
    static Ref<Obj>* keep = new Ref<Obj>(make<Moved>());   // intentionally leaked: lives for the process
    return keep->get();
}

// ---------------------------------------------------------------- copy / compare / streams
Value Interp::copy_value(const Value& v, int depth) {
    auto* r = std::get_if<Ref<Obj>>(&v);
    if (!r || !*r) return v;
    // depth -1: an explicit copy of a named lvalue (copy-initialisation); there a unique_ptr is an error.
    // depth 0 from argument passing / push_back of a temporary: ownership simply transfers.
    bool strict = depth < 0; if (strict) depth = 0;
    if (depth > 256) raise("RecursionError", "value nests too deeply to copy");
    Obj* o = r->get();
    if (o == moved_sentinel()) raise("BorrowError", "use of a moved-from value");
    if (o->own == Own::Unique) { if (strict || depth > 0) raise("BorrowError", "std::unique_ptr cannot be copied; use std::move"); return v; }
    if (o->own == Own::Shared) return v;
    if (auto* s = dynamic_cast<String*>(o)) return new_string(s->s);
    if (auto* l = dynamic_cast<List*>(o)) {
        auto n = make<List>(); n->items.reserve(l->items.size());
        for (auto& x : l->items) n->push(copy_value(x, depth + 1));
        return obj(n);
    }
    if (auto* m = dynamic_cast<Map*>(o)) {
        auto n = make<Map>(); n->sorted = m->sorted; n->has_dflt = m->has_dflt; n->dflt = m->dflt; n->is_set = m->is_set;
        for (auto& [k, x] : m->entries) n->set(k, copy_value(x, depth + 1));
        return obj(n);
    }
    if (auto* i = dynamic_cast<Instance*>(o)) {
        if (!i->cls->is_struct) return v;
        auto c = make<Instance>(i->cls);
        for (auto& [k, x] : i->fields) c->fields[k] = copy_value(x, depth + 1);
        c->finalize = i->cls->find_method("~") != nullptr;   // a copy is a distinct object with its own destructor run
        return obj(c);
    }
    return v;
}

Value Interp::call_operator(const Value& self, const std::string& op, std::vector<Value> args, bool& found) {
    found = false;
    auto* i = as<Instance>(self); if (!i) return Nil{};
    auto* m = i->cls->find_method(op); if (!m) return Nil{};
    found = true;
    Value b = obj(make<Bound>(self, *m));
    return call(b, args);
}

static int rank(const Value& v) {
    if (std::holds_alternative<Nil>(v)) return 0;
    if (std::holds_alternative<bool>(v)) return 1;
    if (std::holds_alternative<std::int64_t>(v) || std::holds_alternative<double>(v)) return 2;
    if (as<String>(v)) return 3;
    if (as<List>(v)) return 4;
    return 5;
}
int Interp::compare(const Value& a, const Value& b) {
    int ra = rank(a), rb = rank(b);
    if (ra == 5 && as<Instance>(a)) {
        bool f = false; Value r = call_operator(a, "operator<=>", {b}, f);
        if (f) { auto* n = std::get_if<std::int64_t>(&r); return n ? int((*n > 0) - (*n < 0)) : 0; }
        r = call_operator(a, "operator<", {b}, f);
        if (f) { if (truthy(r)) return -1; Value r2 = call_operator(b, "operator<", {a}, f); return f && truthy(r2) ? 1 : 0; }
        raise("TypeError", "no operator< for " + as<Instance>(a)->cls->name);
    }
    if (ra != rb) return ra < rb ? -1 : 1;
    switch (ra) {
        case 0: return 0;
        case 1: return int(std::get<bool>(a)) - int(std::get<bool>(b));
        case 2: {
            auto* ia = std::get_if<std::int64_t>(&a); auto* ib = std::get_if<std::int64_t>(&b);
            if (ia && ib) return (*ia > *ib) - (*ia < *ib);
            double x = ia ? double(*ia) : std::get<double>(a), y = ib ? double(*ib) : std::get<double>(b);
            return (x > y) - (x < y);
        }
        case 3: { int c = as<String>(a)->s.compare(as<String>(b)->s); return (c > 0) - (c < 0); }
        case 4: {
            auto& x = as<List>(a)->items; auto& y = as<List>(b)->items;
            for (std::size_t k = 0; k < x.size() && k < y.size(); ++k) if (int c = compare(x[k], y[k])) return c;
            return (x.size() > y.size()) - (x.size() < y.size());
        }
    }
    raise("TypeError", "cannot order " + type_of(a) + " and " + type_of(b));
}
void Interp::sort_keys(std::vector<Value>& keys) {
    std::stable_sort(keys.begin(), keys.end(), [&](const Value& x, const Value& y) { return compare(x, y) < 0; });
}

void Interp::stream_write(Ostream& os, const Value& v) {
    if (auto* m = as<Manip>(v)) {
        if (m->kind == "boolalpha") os.boolalpha = true;
        else if (m->kind == "noboolalpha") os.boolalpha = false;
        else if (m->kind == "fixed") os.fixed = true;
        else if (m->kind == "defaultfloat") { os.fixed = false; os.precision = -1; }
        else if (m->kind == "setprecision") os.precision = int(m->n);
        else if (m->kind == "setw") os.width = int(m->n);
        return;
    }
    std::string s;
    if (auto* b = std::get_if<bool>(&v)) s = os.boolalpha ? (*b ? "true" : "false") : (*b ? "1" : "0");
    else if (auto* d = std::get_if<double>(&v)) {
        if (os.fixed) s = std::format("{:.{}f}", *d, os.precision < 0 ? 6 : os.precision);
        else s = std::format("{:.{}g}", *d, os.precision < 0 ? 6 : std::max(1, os.precision));
    } else s = str(v);
    if (os.width > 0 && int(s.size()) < os.width) s = std::string(std::size_t(os.width) - s.size(), ' ') + s;
    os.width = 0;
    if (os.err) { std::fwrite(s.data(), 1, s.size(), stderr); std::fflush(stderr); }
    else sh->out(s);
}

// ---------------------------------------------------------------- std::format
namespace {
std::string format_one(Interp& in, const Value& v, const std::string& spec) {
    if (spec.empty()) {
        if (auto* d = std::get_if<double>(&v)) return std::format("{}", *d);
        return in.str(v);
    }
    std::string f = "{:" + spec + "}";
    try {
        if (auto* i = std::get_if<std::int64_t>(&v)) return std::vformat(f, std::make_format_args(*i));
        if (auto* d = std::get_if<double>(&v)) return std::vformat(f, std::make_format_args(*d));
        if (auto* b = std::get_if<bool>(&v)) return std::vformat(f, std::make_format_args(*b));
        std::string s = in.str(v);
        return std::vformat(f, std::make_format_args(s));
    } catch (const std::format_error& e) {
        in.raise("ValueError", std::string("std::format: bad spec '{:") + spec + "}' for " + Interp::type_of(v) + ": " + e.what());
    }
}
} // namespace

std::string cpp_format(Interp& in, const std::string& fmt, const std::vector<Value>& args, std::size_t first) {
    std::string out; std::size_t next = first;
    for (std::size_t i = 0; i < fmt.size(); ++i) {
        char c = fmt[i];
        if (c == '{') {
            if (i + 1 < fmt.size() && fmt[i + 1] == '{') { out += '{'; ++i; continue; }
            auto close = fmt.find('}', i);
            if (close == std::string::npos) in.raise("ValueError", "std::format: unmatched '{'");
            std::string body = fmt.substr(i + 1, close - i - 1), spec;
            auto colon = body.find(':');
            if (colon != std::string::npos) { spec = body.substr(colon + 1); body = body.substr(0, colon); }
            std::size_t idx;
            if (!body.empty()) {
                std::size_t n = 0; auto [p, ec] = std::from_chars(body.data(), body.data() + body.size(), n);
                if (ec != std::errc{} || p != body.data() + body.size()) in.raise("ValueError", "std::format: bad argument index '" + body + "'");
                idx = first + n;
            } else idx = next++;
            if (idx >= args.size()) in.raise("ValueError", "std::format: missing argument for '{" + body + "}'");
            out += format_one(in, args[idx], spec);
            i = close;
        } else if (c == '}') {
            if (i + 1 < fmt.size() && fmt[i + 1] == '}') ++i;
            out += '}';
        } else out += c;
    }
    return out;
}

// ---------------------------------------------------------------- install
namespace {
Value shell_call(Interp& in, const std::string& kind, Args& a) {
    // forge::sh("ls {}", dir): literal pieces are passed through, {} arguments are quoted for the shell
    std::string fmt = in.str(a[0]);
    Node body; body.k = NK::Interp; std::vector<Value> parts; std::size_t next = 1; std::string lit;
    auto flush = [&] { auto s = std::make_shared<Node>(); s->k = NK::Str; s->s = lit; body.kids.push_back(s); parts.push_back(sv(lit)); lit.clear(); };
    for (std::size_t i = 0; i < fmt.size(); ++i) {
        if (fmt[i] == '{' && i + 1 < fmt.size() && fmt[i + 1] == '{') { lit += '{'; ++i; continue; }
        if (fmt[i] == '}' && i + 1 < fmt.size() && fmt[i + 1] == '}') { lit += '}'; ++i; continue; }
        if (fmt[i] == '{' && i + 1 < fmt.size() && fmt[i + 1] == '}') {
            if (next >= a.size()) in.raise("ValueError", "forge::" + kind + ": missing argument for '{}'");
            flush(); auto x = std::make_shared<Node>(); x->k = NK::Nil; body.kids.push_back(x); parts.push_back(a[next++]); ++i; continue;
        }
        lit += fmt[i];
    }
    flush();
    Node n; n.k = NK::Shell; n.s = kind; n.kids.push_back(std::make_shared<Node>(std::move(body)));
    return in.shell(n, parts);
}
std::vector<std::string> field_order(const Class* c) {
    std::vector<const Class*> chain; for (; c; c = c->base.get()) chain.push_back(c);
    std::vector<std::string> out; for (auto k = chain.rbegin(); k != chain.rend(); ++k) for (auto& f : (*k)->fields) out.push_back(f);
    return out;
}
} // namespace

std::vector<std::string>& script_argv() { static std::vector<std::string> v; return v; }

void install_cpp_std(Shared& sh) {
    auto& B = sh.builtins;
    auto def = [&](const std::string& n, int lo, int hi, Fn f) { B[n] = nat(n, lo, hi, std::move(f)); };

    // ---- iostream / print / format
    { auto o = make<Ostream>(false); B["std::cout"] = obj(o); B["stdout"] = obj(o); auto e = make<Ostream>(true); B["std::cerr"] = obj(e); B["stderr"] = obj(e); B["std::clog"] = obj(make<Ostream>(true)); }
    B["std::endl"] = sv("\n"); B["std::flush"] = sv("");
    for (const char* m : {"boolalpha", "noboolalpha", "fixed", "defaultfloat"}) B[std::string("std::") + m] = obj(make<Manip>(m, 0));
    def("std::setprecision", 1, 1, [](Interp& in, Args& a) -> Value { return obj(make<Manip>("setprecision", iv(in, a[0], "setprecision"))); });
    def("std::setw", 1, 1, [](Interp& in, Args& a) -> Value { return obj(make<Manip>("setw", iv(in, a[0], "setw"))); });
    auto printer = [&](const std::string& name, bool nl) {
        def(name, 0, -1, [nl](Interp& in, Args& a) -> Value {
            std::size_t first = 0; bool err = false;
            if (!a.empty()) if (auto* os = as<Ostream>(a[0])) { err = os->err; first = 1; }
            std::string s;
            if (a.size() > first) {
                auto* f = as<String>(a[first]); if (!f) in.raise("TypeError", "std::print: the format must be a string literal");
                s = cpp_format(in, f->s, a, first + 1);
            }
            if (nl) s += '\n';
            if (err) { std::fwrite(s.data(), 1, s.size(), stderr); std::fflush(stderr); } else in.sh->out(s);
            return Nil{};
        });
    };
    printer("std::println", true); printer("std::print", false);
    def("std::format", 1, -1, [](Interp& in, Args& a) -> Value { return sv(cpp_format(in, sref(in, a[0], "std::format"), a, 1)); });

    // ---- strings & numbers
    def("std::to_string", 1, 1, [](Interp& in, Args& a) -> Value {
        if (auto* d = std::get_if<double>(&a[0])) return sv(std::to_string(*d));
        if (auto* b = std::get_if<bool>(&a[0])) return sv(*b ? "1" : "0");
        return sv(std::to_string(iv(in, a[0], "std::to_string")));
    });
    auto stoi = [&](const std::string& n) {
        def(n, 1, 2, [n](Interp& in, Args& a) -> Value {
            const std::string& s = sref(in, a[0], n.c_str()); int base = a.size() > 1 ? int(iv(in, a[1], n.c_str())) : 10;
            std::size_t b = s.find_first_not_of(" \t\n\r"); if (b == std::string::npos) in.raise("ValueError", n + ": invalid_argument");
            const char* p = s.data() + b; if (*p == '+') ++p;
            std::int64_t v = 0; auto [e, ec] = std::from_chars(p, s.data() + s.size(), v, base);
            if (ec == std::errc::result_out_of_range) in.raise("OverflowError", n + ": out_of_range");
            if (ec != std::errc{}) in.raise("ValueError", n + ": invalid_argument ('" + s + "')");
            return v;
        });
    };
    for (auto n : {"std::stoi", "std::stol", "std::stoll", "std::stoul", "std::stoull"}) stoi(n);
    for (auto n : {"std::stod", "std::stof", "std::stold"}) def(n, 1, 1, [n = std::string(n)](Interp& in, Args& a) -> Value {
        const std::string& s = sref(in, a[0], n.c_str()); std::size_t b = s.find_first_not_of(" \t\n\r");
        if (b == std::string::npos) in.raise("ValueError", n + ": invalid_argument");
#if defined(__cpp_lib_to_chars) && __cpp_lib_to_chars >= 201611L
        double v = 0; auto [e, ec] = std::from_chars(s.data() + b + (s[b] == '+'), s.data() + s.size(), v);
#else   // older libc++ (e.g. FreeBSD 14 base, LLVM 18) has no floating-point from_chars
        std::string tmp(s.data() + b + (s[b] == '+'), s.data() + s.size());
        std::errc ec{}; double v = 0;
        if (tmp.empty() || tmp[0] == '+' || (tmp[0] == '-' && tmp.size() > 1 && tmp[1] == '+') || std::isspace(static_cast<unsigned char>(tmp[0]))) ec = std::errc::invalid_argument;
        else { char* end = nullptr; errno = 0; v = std::strtod(tmp.c_str(), &end);
               if (end == tmp.c_str()) ec = std::errc::invalid_argument; else if (errno == ERANGE) ec = std::errc::result_out_of_range; }
#endif
        if (ec == std::errc::result_out_of_range) in.raise("OverflowError", n + ": out_of_range");
        if (ec != std::errc{}) in.raise("ValueError", n + ": invalid_argument ('" + s + "')");
        return v;
    });
    def("std::string", 0, 2, [](Interp& in, Args& a) -> Value {
        if (a.empty()) return sv("");
        if (a.size() == 2) { auto n = iv(in, a[0], "std::string"); if (n < 0) in.raise("ValueError", "std::string: negative count"); return sv(std::string(std::size_t(n), sref(in, a[1], "std::string").empty() ? ' ' : sref(in, a[1], "std::string")[0])); }
        if (auto* s = as<String>(a[0])) return sv(s->s);
        return sv(in.str(a[0]));
    });
    B["std::string::npos"] = std::int64_t(-1);
    B["std::nullopt"] = Nil{}; B["nullptr"] = Nil{}; B["std::nullptr_t"] = Nil{};
    def("std::numeric_limits<int>::max", 0, 0, [](Interp&, Args&) -> Value { return std::numeric_limits<std::int64_t>::max(); });
    def("std::numeric_limits<int>::min", 0, 0, [](Interp&, Args&) -> Value { return std::numeric_limits<std::int64_t>::min(); });
    def("std::numeric_limits<int>::lowest", 0, 0, [](Interp&, Args&) -> Value { return std::numeric_limits<std::int64_t>::min(); });
    def("std::numeric_limits<float>::max", 0, 0, [](Interp&, Args&) -> Value { return std::numeric_limits<double>::max(); });
    def("std::numeric_limits<float>::min", 0, 0, [](Interp&, Args&) -> Value { return std::numeric_limits<double>::min(); });
    def("std::numeric_limits<float>::lowest", 0, 0, [](Interp&, Args&) -> Value { return std::numeric_limits<double>::lowest(); });
    def("std::numeric_limits<float>::infinity", 0, 0, [](Interp&, Args&) -> Value { return std::numeric_limits<double>::infinity(); });
    def("std::numeric_limits<float>::epsilon", 0, 0, [](Interp&, Args&) -> Value { return std::numeric_limits<double>::epsilon(); });
    def("std::numeric_limits<float>::quiet_NaN", 0, 0, [](Interp&, Args&) -> Value { return std::numeric_limits<double>::quiet_NaN(); });

    // ---- <cmath> / <algorithm> scalars
    auto m1 = [&](const std::string& n, double (*f)(double)) { def("std::" + n, 1, 1, [f, n](Interp& in, Args& a) -> Value { return f(dv(in, a[0], n.c_str())); }); };
    m1("sqrt", [](double x) { return std::sqrt(x); }); m1("cbrt", [](double x) { return std::cbrt(x); });
    m1("floor", [](double x) { return std::floor(x); }); m1("ceil", [](double x) { return std::ceil(x); });
    m1("round", [](double x) { return std::round(x); }); m1("trunc", [](double x) { return std::trunc(x); });
    m1("fabs", [](double x) { return std::fabs(x); }); m1("log", [](double x) { return std::log(x); });
    m1("log2", [](double x) { return std::log2(x); }); m1("log10", [](double x) { return std::log10(x); });
    m1("exp", [](double x) { return std::exp(x); }); m1("sin", [](double x) { return std::sin(x); });
    m1("cos", [](double x) { return std::cos(x); }); m1("tan", [](double x) { return std::tan(x); });
    m1("atan", [](double x) { return std::atan(x); });
    def("std::pow", 2, 2, [](Interp& in, Args& a) -> Value { return std::pow(dv(in, a[0], "pow"), dv(in, a[1], "pow")); });
    def("std::atan2", 2, 2, [](Interp& in, Args& a) -> Value { return std::atan2(dv(in, a[0], "atan2"), dv(in, a[1], "atan2")); });
    def("std::hypot", 2, 2, [](Interp& in, Args& a) -> Value { return std::hypot(dv(in, a[0], "hypot"), dv(in, a[1], "hypot")); });
    def("std::fmod", 2, 2, [](Interp& in, Args& a) -> Value { return std::fmod(dv(in, a[0], "fmod"), dv(in, a[1], "fmod")); });
    def("std::isnan", 1, 1, [](Interp& in, Args& a) -> Value { return std::isnan(dv(in, a[0], "isnan")); });
    def("std::isinf", 1, 1, [](Interp& in, Args& a) -> Value { return std::isinf(dv(in, a[0], "isinf")); });
    def("std::abs", 1, 1, [](Interp& in, Args& a) -> Value {
        if (auto* i = std::get_if<std::int64_t>(&a[0])) { if (*i == std::numeric_limits<std::int64_t>::min()) in.raise("OverflowError", "std::abs overflow"); return *i < 0 ? -*i : *i; }
        return std::fabs(dv(in, a[0], "std::abs"));
    });
    auto minmax = [&](const std::string& n, bool mx) {
        def(n, 1, -1, [mx, n](Interp& in, Args& a) -> Value {
            std::vector<Value> xs; Value cmp;
            if (a.size() == 1) { xs = lref(in, a[0], n.c_str()).items; }
            else if (a.size() == 3 && !std::holds_alternative<std::int64_t>(a[2]) && !std::holds_alternative<double>(a[2]) && !as<String>(a[2])) { xs = {a[0], a[1]}; cmp = a[2]; }
            else xs = a;
            if (xs.empty()) in.raise("ValueError", n + " of an empty list");
            auto less = [&](const Value& x, const Value& y) { if (!is_nil(cmp)) { Args c{x, y}; return Interp::truthy(in.call(cmp, c)); } return in.compare(x, y) < 0; };
            Value best = xs[0];
            for (std::size_t k = 1; k < xs.size(); ++k) if (mx ? less(best, xs[k]) : less(xs[k], best)) best = xs[k];
            return best;
        });
    };
    minmax("std::min", false); minmax("std::max", true);
    def("std::clamp", 3, 3, [](Interp& in, Args& a) -> Value { if (in.compare(a[0], a[1]) < 0) return a[1]; if (in.compare(a[2], a[0]) < 0) return a[2]; return a[0]; });
    def("std::size", 1, 1, [](Interp&, Args& a) -> Value { return clen(a[0]); });
    def("std::ssize", 1, 1, [](Interp&, Args& a) -> Value { return clen(a[0]); });
    def("std::empty", 1, 1, [](Interp&, Args& a) -> Value { return clen(a[0]) == 0; });
    auto ch = [&](const std::string& n, int (*f)(int), bool to_char) {
        def("std::" + n, 1, 1, [f, to_char, n](Interp& in, Args& a) -> Value {
            int c = as<String>(a[0]) ? (sref(in, a[0], n.c_str()).empty() ? 0 : (unsigned char)sref(in, a[0], n.c_str())[0]) : int(iv(in, a[0], n.c_str()));
            int r = f(c); if (to_char) return sv(std::string(1, char(r))); return r != 0;
        });
    };
    ch("isdigit", [](int c) { return std::isdigit(c); }, false); ch("isalpha", [](int c) { return std::isalpha(c); }, false);
    ch("isalnum", [](int c) { return std::isalnum(c); }, false); ch("isspace", [](int c) { return std::isspace(c); }, false);
    ch("isupper", [](int c) { return std::isupper(c); }, false); ch("islower", [](int c) { return std::islower(c); }, false);
    ch("ispunct", [](int c) { return std::ispunct(c); }, false);
    ch("toupper", [](int c) { return std::toupper(c); }, true); ch("tolower", [](int c) { return std::tolower(c); }, true);

    // ---- <algorithm> / <numeric> over v.begin()/v.end() or a whole container (std::ranges::)
    auto with_cmp = [](Interp& in, const Value& cmp) {
        return [&in, cmp](const Value& x, const Value& y) {
            if (is_nil(cmp)) return in.compare(x, y) < 0;
            Args c{x, y}; return Interp::truthy(in.call(cmp, c));
        };
    };
    for (std::string pre : {"std::", "std::ranges::"}) {
        def(pre + "sort", 1, 3, [with_cmp](Interp& in, Args& a) -> Value {
            std::size_t nx; Span s = range_args(in, a, nx, "std::sort"); auto& l = span_list(in, s, "std::sort"); mutable_(in, l);
            Value cmp = a.size() > nx ? a[nx] : Value(Nil{});
            std::stable_sort(l.items.begin() + s.lo, l.items.begin() + s.hi, with_cmp(in, cmp)); return Nil{};
        });
        B[pre + "stable_sort"] = B[pre + "sort"];
        def(pre + "reverse", 1, 2, [](Interp& in, Args& a) -> Value {
            std::size_t nx; Span s = range_args(in, a, nx, "std::reverse");
            if (auto* str = as<String>(s.c)) { mutable_(in, *str); std::reverse(str->s.begin() + s.lo, str->s.begin() + s.hi); return Nil{}; }
            auto& l = span_list(in, s, "std::reverse"); mutable_(in, l); std::reverse(l.items.begin() + s.lo, l.items.begin() + s.hi); return Nil{};
        });
        def(pre + "find", 2, 3, [](Interp& in, Args& a) -> Value {
            std::size_t nx; Span s = range_args(in, a, nx, "std::find");
            for (auto k = s.lo; k < s.hi; ++k) if (in.equal(elem(in, s.c, k), a[nx])) return it(s.c, k);
            return it(s.c, s.hi);
        });
        def(pre + "find_if", 2, 3, [](Interp& in, Args& a) -> Value {
            std::size_t nx; Span s = range_args(in, a, nx, "std::find_if");
            for (auto k = s.lo; k < s.hi; ++k) { Args c{elem(in, s.c, k)}; if (Interp::truthy(in.call(a[nx], c))) return it(s.c, k); }
            return it(s.c, s.hi);
        });
        def(pre + "count", 2, 3, [](Interp& in, Args& a) -> Value {
            std::size_t nx; Span s = range_args(in, a, nx, "std::count"); std::int64_t n = 0;
            for (auto k = s.lo; k < s.hi; ++k) n += in.equal(elem(in, s.c, k), a[nx]); return n;
        });
        def(pre + "count_if", 2, 3, [](Interp& in, Args& a) -> Value {
            std::size_t nx; Span s = range_args(in, a, nx, "std::count_if"); std::int64_t n = 0;
            for (auto k = s.lo; k < s.hi; ++k) { Args c{elem(in, s.c, k)}; n += Interp::truthy(in.call(a[nx], c)); } return n;
        });
        auto pred = [&](const std::string& n, int mode) {   // 0 any, 1 all, 2 none
            def(pre + n, 2, 3, [mode](Interp& in, Args& a) -> Value {
                std::size_t nx; Span s = range_args(in, a, nx, "std::any_of");
                for (auto k = s.lo; k < s.hi; ++k) {
                    Args c{elem(in, s.c, k)}; bool t = Interp::truthy(in.call(a[nx], c));
                    if (mode == 0 && t) return true; if (mode == 1 && !t) return false; if (mode == 2 && t) return false;
                }
                return mode != 0;
            });
        };
        pred("any_of", 0); pred("all_of", 1); pred("none_of", 2);
        def(pre + "for_each", 2, 3, [](Interp& in, Args& a) -> Value {
            std::size_t nx; Span s = range_args(in, a, nx, "std::for_each");
            for (auto k = s.lo; k < s.hi; ++k) { Args c{elem(in, s.c, k)}; in.call_ref(a[nx], c); if (auto* l = as<List>(s.c); l && std::size_t(k) < l->items.size()) l->items[std::size_t(k)] = c[0]; } return Nil{};
        });
        auto ext = [&](const std::string& n, bool mx) {
            def(pre + n, 1, 3, [mx, with_cmp](Interp& in, Args& a) -> Value {
                std::size_t nx; Span s = range_args(in, a, nx, "std::max_element"); auto less = with_cmp(in, a.size() > nx ? a[nx] : Value(Nil{}));
                if (s.lo >= s.hi) return it(s.c, s.hi);
                auto best = s.lo;
                for (auto k = s.lo + 1; k < s.hi; ++k) { Value x = elem(in, s.c, k), b = elem(in, s.c, best); if (mx ? less(b, x) : less(x, b)) best = k; }
                return it(s.c, best);
            });
        };
        ext("max_element", true); ext("min_element", false);
        def(pre + "fill", 2, 3, [](Interp& in, Args& a) -> Value {
            std::size_t nx; Span s = range_args(in, a, nx, "std::fill"); auto& l = span_list(in, s, "std::fill"); mutable_(in, l);
            for (auto k = s.lo; k < s.hi; ++k) l.items[std::size_t(k)] = in.copy_value(a[nx]); return Nil{};
        });
        def(pre + "iota", 2, 3, [](Interp& in, Args& a) -> Value {
            std::size_t nx; Span s = range_args(in, a, nx, "std::iota"); auto& l = span_list(in, s, "std::iota"); mutable_(in, l);
            Value v = a[nx]; for (auto k = s.lo; k < s.hi; ++k) { l.items[std::size_t(k)] = v; v = in.binary(Op::Add, v, std::int64_t(1)); } return Nil{};
        });
        def(pre + "binary_search", 2, 3, [](Interp& in, Args& a) -> Value {
            std::size_t nx; Span s = range_args(in, a, nx, "std::binary_search");
            auto lo = s.lo, hi = s.hi;
            while (lo < hi) { auto mid = lo + (hi - lo) / 2; int c = in.compare(elem(in, s.c, mid), a[nx]); if (c == 0) return true; if (c < 0) lo = mid + 1; else hi = mid; }
            return false;
        });
        def(pre + "transform", 3, 4, [](Interp& in, Args& a) -> Value {   // transform(b, e, out_b, f): out may be the same range
            if (a.size() == 4) {
                Span s = span(in, a[0], a[1], "std::transform"); auto* ob = as<PosIt>(a[2]); if (!ob) in.raise("TypeError", "std::transform: output must be an iterator (use v.begin())");
                auto& out = lref(in, ob->c, "std::transform"); mutable_(in, out);
                for (auto k = s.lo; k < s.hi; ++k) {
                    Args c{elem(in, s.c, k)}; Value r = in.call(a[3], c); auto o = std::size_t(ob->i + (k - s.lo));
                    if (o < out.items.size()) out.items[o] = r; else out.push(r);
                }
                return it(ob->c, ob->i + (s.hi - s.lo));
            }
            in.raise("TypeError", "std::transform needs (begin, end, out, fn)");
        });
    }
    def("std::accumulate", 3, 4, [](Interp& in, Args& a) -> Value {
        Span s = span(in, a[0], a[1], "std::accumulate"); Value acc = a[2];
        for (auto k = s.lo; k < s.hi; ++k) { if (a.size() > 3) { Args c{acc, elem(in, s.c, k)}; acc = in.call(a[3], c); } else acc = in.binary(Op::Add, acc, elem(in, s.c, k)); }
        return acc;
    });
    B["std::reduce"] = B["std::accumulate"];
    def("std::distance", 2, 2, [](Interp& in, Args& a) -> Value { Span s = span(in, a[0], a[1], "std::distance"); return as<PosIt>(a[1])->i - as<PosIt>(a[0])->i; (void)s; });
    def("std::next", 1, 2, [](Interp& in, Args& a) -> Value { auto* p = as<PosIt>(a[0]); if (!p) in.raise("TypeError", "std::next needs an iterator"); return it(p->c, p->i + (a.size() > 1 ? iv(in, a[1], "next") : 1)); });
    def("std::prev", 1, 2, [](Interp& in, Args& a) -> Value { auto* p = as<PosIt>(a[0]); if (!p) in.raise("TypeError", "std::prev needs an iterator"); return it(p->c, p->i - (a.size() > 1 ? iv(in, a[1], "prev") : 1)); });
    def("std::begin", 1, 1, [](Interp&, Args& a) -> Value { return it(a[0], 0); });
    def("std::end", 1, 1, [](Interp&, Args& a) -> Value { return it(a[0], clen(a[0])); });
    def("std::make_pair", 2, 2, [](Interp& in, Args& a) -> Value { return obj(make<List>(std::vector<Value>{in.copy_value(a[0]), in.copy_value(a[1])})); });
    def("std::make_tuple", 0, -1, [](Interp& in, Args& a) -> Value { auto l = make<List>(); for (auto& x : a) l->push(in.copy_value(x)); return obj(l); });
    B["std::tie"] = B["std::make_tuple"];
    def("std::vector", 0, 2, [](Interp& in, Args& a) -> Value {
        auto l = make<List>(); if (a.empty()) return obj(l);
        if (auto* src = as<List>(a[0])) { for (auto& x : src->items) l->push(in.copy_value(x)); return obj(l); }
        auto n = iv(in, a[0], "std::vector"); if (n < 0 || n > (std::int64_t(1) << 32)) in.raise("ValueError", "std::vector: bad size");
        Value fill = a.size() > 1 ? a[1] : Value(std::int64_t(0));
        for (std::int64_t k = 0; k < n; ++k) l->push(in.copy_value(fill)); return obj(l);
    });

    // ---- forge:: helpers the parser lowers to
    def("forge::__make_map", 3, 3, [](Interp& in, Args& a) -> Value {
        auto m = make<Map>(); auto kind = iv(in, a[0], "__make_map");
        m->sorted = kind & 1; m->is_set = kind >= 2;
        if (!is_nil(a[1])) { m->has_dflt = true; m->dflt = a[1]; }
        auto& l = lref(in, a[2], "__make_map");
        for (std::size_t k = 0; k + 1 < l.items.size(); k += 2) if (!m->find(l.items[k])) m->set(l.items[k], l.items[k + 1]);
        if (m->sorted) {
            std::vector<Value> ks; for (auto& e : m->entries) ks.push_back(e.first);
            in.sort_keys(ks);
            auto n = make<Map>(); n->sorted = true; n->is_set = m->is_set; n->has_dflt = m->has_dflt; n->dflt = m->dflt;
            for (auto& k : ks) n->set(k, *m->find(k));
            return obj(n);
        }
        return obj(m);
    });
    def("forge::__cast", 2, 2, [](Interp& in, Args& a) -> Value {
        const std::string& t = sref(in, a[0], "cast"); const Value& v = a[1];
        if (t == "string") { if (auto* s = as<String>(v)) return sv(s->s); return sv(in.str(v)); }
        if (t == "bool") return Interp::truthy(v);
        if (auto* s = as<String>(v)) { if (s->s.size() == 1) { int c = (signed char)s->s[0]; return t == "float" ? Value(double(c)) : Value(std::int64_t(c)); } }
        if (t == "float") return dv(in, v, "static_cast<double>");
        if (auto* d = std::get_if<double>(&v)) {
            if (!std::isfinite(*d) || *d >= 9.2233720368547758e18 || *d < -9.2233720368547758e18) in.raise("OverflowError", "double out of range for int conversion");
            return std::int64_t(*d);
        }
        return iv(in, v, "static_cast<int>");
    });
    def("forge::__own", 2, 2, [](Interp& in, Args& a) -> Value {
        auto* r = std::get_if<Ref<Obj>>(&a[1]);
        if (!r || !*r) { // make_shared<int>(5): box scalars so the pointer has identity
            auto l = make<SharedBox>(a[1]); l->own = iv(in, a[0], "own") == 1 ? Own::Shared : Own::Unique; return obj(l);
        }
        (*r)->own = iv(in, a[0], "own") == 1 ? Own::Shared : Own::Unique; return a[1];
    });
    def("forge::__get", 2, 2, [](Interp& in, Args& a) -> Value {
        if (auto* i = as<Instance>(a[0])) {
            auto fs = field_order(i->cls.get()); auto k = iv(in, a[1], "get");
            if (k < 0 || std::size_t(k) >= fs.size()) in.raise("IndexError", "structured binding index out of range");
            return in.get_field(a[0], fs[std::size_t(k)]);
        }
        if (auto* s = as<String>(a[1])) in.raise("TypeError", "std::get<" + s->s + "> by type is not supported; use an index");
        return in.get_index(a[0], a[1]);
    });
    def("forge::__deref", 1, 1, [](Interp& in, Args& a) -> Value {
        if (is_nil(a[0])) in.raise("NullPointerError", "dereferencing a null pointer (nullptr / empty optional)");
        if (auto* p = as<PosIt>(a[0])) return elem(in, p->c, p->i);
        if (auto* b = as<SharedBox>(a[0])) { std::lock_guard g(b->m); return b->v; }
        return a[0];
    });
    def("forge::__weak", 1, 1, [&sh](Interp& in, Args& a) -> Value { if (is_nil(a[0])) return Nil{}; Args x{a[0]}; return in.call(sh.builtins["weak"], x); });
    def("forge::__downcast", 2, 2, [](Interp&, Args& a) -> Value {
        auto* i = as<Instance>(a[1]); auto* c = as<Class>(a[0]);
        if (!i || !c) return Nil{};
        for (const Class* k = i->cls.get(); k; k = k->base.get()) if (k == c) return a[1];
        return Nil{};
    });
    def("forge::__cmp3", 2, 2, [](Interp& in, Args& a) -> Value { return std::int64_t(in.compare(a[0], a[1])); });
    def("forge::__using_namespace", 1, 1, [](Interp& in, Args& a) -> Value {
        auto& u = in.globals->using_ns; std::string n = sref(in, a[0], "using");
        if (std::find(u.begin(), u.end(), n) == u.end()) u.push_back(n); return Nil{};
    });
    def("forge::spawn", 1, -1, [](Interp& in, Args& a) -> Value { Value f = a[0]; std::vector<Value> r(a.begin() + 1, a.end()); return in.spawn(f, std::move(r)); });
    def("std::async", 1, -1, [](Interp& in, Args& a) -> Value {
        std::size_t s = as<Manip>(a[0]) ? 1 : 0; if (s >= a.size()) in.raise("TypeError", "std::async needs a function");
        Value f = a[s]; std::vector<Value> r(a.begin() + std::ptrdiff_t(s) + 1, a.end()); return in.spawn(f, std::move(r));
    });
    B["std::launch::async"] = obj(make<Manip>("launch", 0)); B["std::launch::deferred"] = B["std::launch::async"];
    def("forge::await", 1, 1, [](Interp& in, Args& a) -> Value { return in.await(a[0]); });
    def("forge::shared", 1, 1, [](Interp& in, Args& a) -> Value { return obj(make<SharedBox>(in.copy_value(a[0]))); });
    def("forge::args", 0, 0, [](Interp&, Args&) -> Value { auto l = make<List>(); for (auto& s : script_argv()) l->push(sv(s)); return obj(l); });
    for (auto& b : shells().all()) {   // pluggable backends (script/src/shells/*.cpp): forge::<name> and aliases
        std::vector<std::string> ns{b.name}; ns.insert(ns.end(), b.aliases.begin(), b.aliases.end());
        for (auto& k : ns) def("forge::" + k, 1, -1, [k](Interp& in, Args& a) -> Value { return shell_call(in, k, a); });
    }
    // forge::shell: the host's default shell (FORGE_SHELL, else pwsh/powershell/cmd on Windows, mksh on Android, sh elsewhere)
    def("forge::shell", 1, -1, [](Interp& in, Args& a) -> Value { return shell_call(in, host_shell(), a); });
    def("forge::host_shell", 0, 0, [](Interp&, Args&) -> Value { return sv(host_shell()); });
    def("std::getenv", 1, 1, [](Interp& in, Args& a) -> Value {
        if (!in.caps.env) in.raise("PermissionError", "environment access is disabled in this sandbox");   // same gate as os.env
        auto k = in.str(a[0]); enforce_scope(in, SecScope::env, k); if (!os::has_env(k)) return Nil{}; return sv(os::getenv(k)); });
    def("forge::shells", 0, 0, [](Interp&, Args&) -> Value {
        auto l = make<List>();
        for (auto& b : shells().all()) {
            auto m = make<Map>(); std::string exe = shell_exe(b);
            m->set_str("name", sv(b.name)); m->set_str("family", sv(b.family)); m->set_str("available", !exe.empty()); m->set_str("exe", sv(exe));
            m->set_str("tested", b.golden_tested); m->set_str("emit", bool(b.emitter)); m->set_str("emit_tested", b.emit_tested); auto al = make<List>(); for (auto& x : b.aliases) al->push(sv(x)); m->set_str("aliases", obj(al));
            l->push(obj(m));
        }
        return obj(l);
    });
    def("std::chrono::milliseconds", 1, 1, [](Interp& in, Args& a) -> Value { return iv(in, a[0], "milliseconds"); });
    def("std::chrono::seconds", 1, 1, [](Interp& in, Args& a) -> Value { return iv(in, a[0], "seconds") * 1000; });
    def("std::this_thread::sleep_for", 1, 1, [&sh](Interp& in, Args& a) -> Value { Args x{a[0]}; return in.call(sh.builtins["sleep"], x); });
    def("std::this_thread::yield", 0, 0, [](Interp& in, Args&) -> Value { in.step(); std::this_thread::yield(); return Nil{}; });
    def("std::exit", 1, 1, [](Interp& in, Args& a) -> Value { in.raise("SystemExit", std::to_string(iv(in, a[0], "exit"))); });
    def("std::swap", 2, 2, [](Interp& in, Args&) -> Value { in.raise("TypeError", "std::swap must be used as a statement: std::swap(a, b);"); });

    // ---- exceptions: std::runtime_error etc. are the script error classes
    const std::pair<const char*, const char*> errs[] = {
        {"std::exception", "Error"}, {"std::runtime_error", "RuntimeError"}, {"std::logic_error", "LogicError"},
        {"std::invalid_argument", "ValueError"}, {"std::out_of_range", "IndexError"}, {"std::length_error", "ValueError"},
        {"std::domain_error", "ValueError"}, {"std::range_error", "ValueError"}, {"std::overflow_error", "OverflowError"},
        {"std::bad_alloc", "OutOfMemoryError"}, {"std::system_error", "OSError"}};
    for (auto& [cpp, ours] : errs) if (auto e = sh.errors.find(ours); e != sh.errors.end()) B[cpp] = obj(e->second);
    if (auto e = sh.errors.find("Error"); e != sh.errors.end())
        e->second->methods["what"] = nat("what", 1, 1, [](Interp& in, Args& a) -> Value { return in.get_field(a[0], "message"); });

    // ---- C++ member functions on built-in values
    auto& M = sh.methods;
    auto meth = [&](const char* table, const std::string& n, int lo, int hi, Fn f) { M[table][n] = nat(std::string(table) + "." + n, lo, hi, std::move(f)); };
    auto idx = [](Interp& in, const Value& v, std::size_t n, const char* w, bool allow_end = false) {
        std::int64_t i = as<PosIt>(v) ? as<PosIt>(v)->i : iv(in, v, w);
        if (i < 0 || std::size_t(i) > n || (!allow_end && std::size_t(i) == n)) in.raise("IndexError", std::string(w) + ": index " + std::to_string(i) + " out of range for size " + std::to_string(n));
        return std::size_t(i);
    };
    // std::vector
    meth("list", "size", 1, 1, [](Interp&, Args& a) -> Value { return std::int64_t(as<List>(a[0])->items.size()); });
    meth("list", "empty", 1, 1, [](Interp&, Args& a) -> Value { return as<List>(a[0])->items.empty(); });
    meth("list", "push_back", 2, 2, [](Interp& in, Args& a) -> Value { auto* l = as<List>(a[0]); mutable_(in, *l); l->push(in.copy_value(a[1])); return Nil{}; });
    meth("list", "emplace_back", 1, -1, [](Interp& in, Args& a) -> Value {
        auto* l = as<List>(a[0]); mutable_(in, *l);
        if (a.size() == 2) l->push(in.copy_value(a[1]));
        else { auto t = make<List>(); for (std::size_t k = 1; k < a.size(); ++k) t->push(in.copy_value(a[k])); l->push(obj(t)); }
        return l->items.back();
    });
    meth("list", "pop_back", 1, 1, [](Interp& in, Args& a) -> Value { auto* l = as<List>(a[0]); mutable_(in, *l); if (l->items.empty()) in.raise("IndexError", "pop_back on an empty vector"); l->items.pop_back(); return Nil{}; });
    meth("list", "back", 1, 1, [](Interp& in, Args& a) -> Value { auto* l = as<List>(a[0]); if (l->items.empty()) in.raise("IndexError", "back() on an empty vector"); return l->items.back(); });
    meth("list", "front", 1, 1, [](Interp& in, Args& a) -> Value { auto* l = as<List>(a[0]); if (l->items.empty()) in.raise("IndexError", "front() on an empty vector"); return l->items.front(); });
    meth("list", "at", 2, 2, [idx](Interp& in, Args& a) -> Value { auto* l = as<List>(a[0]); return l->items[idx(in, a[1], l->items.size(), "vector::at")]; });
    meth("list", "resize", 2, 3, [](Interp& in, Args& a) -> Value {
        auto* l = as<List>(a[0]); mutable_(in, *l); auto n = iv(in, a[1], "resize"); if (n < 0 || n > (std::int64_t(1) << 32)) in.raise("ValueError", "resize: bad size");
        while (std::int64_t(l->items.size()) > n) l->items.pop_back();
        while (std::int64_t(l->items.size()) < n) l->push(a.size() > 2 ? in.copy_value(a[2]) : Value(std::int64_t(0)));
        return Nil{};
    });
    meth("list", "reserve", 2, 2, [](Interp&, Args&) -> Value { return Nil{}; });
    meth("list", "shrink_to_fit", 1, 1, [](Interp&, Args&) -> Value { return Nil{}; });
    meth("list", "capacity", 1, 1, [](Interp&, Args& a) -> Value { return std::int64_t(as<List>(a[0])->items.capacity()); });
    meth("list", "begin", 1, 1, [](Interp&, Args& a) -> Value { return it(a[0], 0); });
    meth("list", "end", 1, 1, [](Interp&, Args& a) -> Value { return it(a[0], clen(a[0])); });
    meth("list", "insert", 3, 3, [idx](Interp& in, Args& a) -> Value {
        auto* l = as<List>(a[0]); mutable_(in, *l); auto p = idx(in, a[1], l->items.size(), "insert", true);
        l->items.insert(l->items.begin() + std::ptrdiff_t(p), in.copy_value(a[2])); return it(a[0], std::int64_t(p));
    });
    meth("list", "erase", 2, 3, [idx](Interp& in, Args& a) -> Value {
        auto* l = as<List>(a[0]); mutable_(in, *l); auto p = idx(in, a[1], l->items.size(), "erase");
        auto q = a.size() > 2 ? idx(in, a[2], l->items.size(), "erase", true) : p + 1;
        if (q < p) q = p;
        l->items.erase(l->items.begin() + std::ptrdiff_t(p), l->items.begin() + std::ptrdiff_t(q)); return it(a[0], std::int64_t(p));
    });
    meth("list", "swap", 2, 2, [](Interp& in, Args& a) -> Value { auto* l = as<List>(a[0]); auto& o = lref(in, a[1], "swap"); mutable_(in, *l); mutable_(in, o); std::swap(l->items, o.items); return Nil{}; });
    meth("list", "assign", 3, 3, [](Interp& in, Args& a) -> Value { auto* l = as<List>(a[0]); mutable_(in, *l); l->items.clear(); auto n = iv(in, a[1], "assign"); for (std::int64_t k = 0; k < n; ++k) l->push(in.copy_value(a[2])); return Nil{}; });

    // std::string (strings are mutable values; literals are fresh copies)
    meth("string", "size", 1, 1, [](Interp&, Args& a) -> Value { return std::int64_t(as<String>(a[0])->s.size()); });
    M["string"]["length"] = M["string"]["size"];
    meth("string", "empty", 1, 1, [](Interp&, Args& a) -> Value { return as<String>(a[0])->s.empty(); });
    meth("string", "c_str", 1, 1, [](Interp&, Args& a) -> Value { return a[0]; });
    M["string"]["data"] = M["string"]["c_str"];
    meth("string", "substr", 1, 3, [](Interp& in, Args& a) -> Value {
        auto& s = as<String>(a[0])->s; auto p = a.size() > 1 ? iv(in, a[1], "substr") : 0;
        if (p < 0 || std::size_t(p) > s.size()) in.raise("IndexError", "substr: position out of range");
        std::int64_t n = a.size() > 2 ? iv(in, a[2], "substr") : -1;
        return sv(s.substr(std::size_t(p), n < 0 ? std::string::npos : std::size_t(n)));
    });
    auto finder = [&](const std::string& n, int mode) {
        meth("string", n, 2, 3, [mode](Interp& in, Args& a) -> Value {
            auto& s = as<String>(a[0])->s; std::string t = as<String>(a[1]) ? as<String>(a[1])->s : in.str(a[1]);
            std::size_t pos = a.size() > 2 ? std::size_t(std::max<std::int64_t>(0, iv(in, a[2], "find"))) : (mode == 1 || mode == 3 ? std::string::npos : 0);
            std::size_t r = mode == 0 ? s.find(t, pos) : mode == 1 ? s.rfind(t, pos) : mode == 2 ? s.find_first_of(t, pos) : mode == 3 ? s.find_last_of(t, pos) : s.find_first_not_of(t, pos);
            return r == std::string::npos ? std::int64_t(-1) : std::int64_t(r);
        });
    };
    finder("find", 0); finder("rfind", 1); finder("find_first_of", 2); finder("find_last_of", 3); finder("find_first_not_of", 4);
    meth("string", "at", 2, 2, [idx](Interp& in, Args& a) -> Value { auto& s = as<String>(a[0])->s; return sv(std::string(1, s[idx(in, a[1], s.size(), "string::at")])); });
    meth("string", "front", 1, 1, [](Interp& in, Args& a) -> Value { auto& s = as<String>(a[0])->s; if (s.empty()) in.raise("IndexError", "front() on an empty string"); return sv(s.substr(0, 1)); });
    meth("string", "back", 1, 1, [](Interp& in, Args& a) -> Value { auto& s = as<String>(a[0])->s; if (s.empty()) in.raise("IndexError", "back() on an empty string"); return sv(s.substr(s.size() - 1)); });
    meth("string", "push_back", 2, 2, [](Interp& in, Args& a) -> Value { auto* s = as<String>(a[0]); mutable_(in, *s); s->s += sref(in, a[1], "push_back"); return Nil{}; });
    meth("string", "append", 2, 3, [](Interp& in, Args& a) -> Value {
        auto* s = as<String>(a[0]); mutable_(in, *s);
        if (a.size() == 3) { auto n = iv(in, a[1], "append"); auto& c = sref(in, a[2], "append"); for (std::int64_t k = 0; k < n; ++k) s->s += c; }
        else s->s += as<String>(a[1]) ? as<String>(a[1])->s : in.str(a[1]);
        return a[0];
    });
    meth("string", "pop_back", 1, 1, [](Interp& in, Args& a) -> Value { auto* s = as<String>(a[0]); mutable_(in, *s); if (s->s.empty()) in.raise("IndexError", "pop_back on an empty string"); s->s.pop_back(); return Nil{}; });
    meth("string", "clear", 1, 1, [](Interp& in, Args& a) -> Value { auto* s = as<String>(a[0]); mutable_(in, *s); s->s.clear(); return Nil{}; });
    meth("string", "erase", 1, 3, [](Interp& in, Args& a) -> Value {
        auto* s = as<String>(a[0]); mutable_(in, *s); auto p = a.size() > 1 ? iv(in, a[1], "erase") : 0;
        if (p < 0 || std::size_t(p) > s->s.size()) in.raise("IndexError", "erase: position out of range");
        auto n = a.size() > 2 ? iv(in, a[2], "erase") : -1; s->s.erase(std::size_t(p), n < 0 ? std::string::npos : std::size_t(n)); return a[0];
    });
    meth("string", "insert", 3, 3, [](Interp& in, Args& a) -> Value {
        auto* s = as<String>(a[0]); mutable_(in, *s); auto p = iv(in, a[1], "insert");
        if (p < 0 || std::size_t(p) > s->s.size()) in.raise("IndexError", "insert: position out of range");
        s->s.insert(std::size_t(p), sref(in, a[2], "insert")); return a[0];
    });
    meth("string", "replace", 3, 4, [](Interp& in, Args& a) -> Value {
        auto* s = as<String>(a[0]);
        if (a.size() == 3 && as<String>(a[1])) {   // FORGE extension: replace(old, new) -> new string
            std::string r = s->s, from = as<String>(a[1])->s, to = sref(in, a[2], "replace");
            if (from.empty()) return sv(r);
            for (std::size_t p = 0; (p = r.find(from, p)) != std::string::npos; p += to.size()) r.replace(p, from.size(), to);
            return sv(r);
        }
        mutable_(in, *s); auto p = iv(in, a[1], "replace"), n = iv(in, a[2], "replace");
        if (p < 0 || std::size_t(p) > s->s.size() || n < 0) in.raise("IndexError", "replace: position out of range");
        s->s.replace(std::size_t(p), std::size_t(n), sref(in, a[3], "replace")); return a[0];
    });
    meth("string", "compare", 2, 2, [](Interp& in, Args& a) -> Value { int c = as<String>(a[0])->s.compare(sref(in, a[1], "compare")); return std::int64_t((c > 0) - (c < 0)); });
    meth("string", "begin", 1, 1, [](Interp&, Args& a) -> Value { return it(a[0], 0); });
    meth("string", "end", 1, 1, [](Interp&, Args& a) -> Value { return it(a[0], clen(a[0])); });

    // std::map / unordered_map / set
    meth("map", "size", 1, 1, [](Interp&, Args& a) -> Value { return std::int64_t(as<Map>(a[0])->entries.size()); });
    meth("map", "empty", 1, 1, [](Interp&, Args& a) -> Value { return as<Map>(a[0])->entries.empty(); });
    meth("map", "contains", 2, 2, [](Interp&, Args& a) -> Value { return as<Map>(a[0])->find(a[1]) != nullptr; });
    meth("map", "count", 2, 2, [](Interp&, Args& a) -> Value { return std::int64_t(as<Map>(a[0])->find(a[1]) != nullptr); });
    meth("map", "at", 2, 2, [](Interp& in, Args& a) -> Value { if (auto* v = as<Map>(a[0])->find(a[1])) return *v; in.raise("KeyError", "map::at: key " + in.repr(a[1]) + " not found"); });
    auto put = [](Interp& in, Map& m, const Value& k, Value v, bool overwrite) {
        mutable_(in, m);
        if (m.find(k)) { if (overwrite) m.set(k, std::move(v)); return false; }
        if (m.sorted && !m.entries.empty() && in.compare(m.entries.back().first, k) > 0) {   // keep key order: rebuild
            std::vector<std::pair<Value, Value>> es = m.entries; es.emplace_back(k, std::move(v));
            std::stable_sort(es.begin(), es.end(), [&](auto& x, auto& y) { return in.compare(x.first, y.first) < 0; });
            m.entries.clear(); m.index.clear(); for (auto& [a2, b2] : es) m.set(a2, b2);
        } else m.set(k, std::move(v));
        return true;
    };
    meth("map", "insert", 2, 2, [put](Interp& in, Args& a) -> Value {
        auto* m = as<Map>(a[0]);
        if (m->is_set) return put(in, *m, in.copy_value(a[1]), true, false);
        auto* p = as<List>(a[1]); if (!p || p->items.size() != 2) in.raise("TypeError", "map::insert expects a {key, value} pair");
        return put(in, *m, in.copy_value(p->items[0]), in.copy_value(p->items[1]), false);
    });
    meth("map", "emplace", 2, 3, [put](Interp& in, Args& a) -> Value {
        auto* m = as<Map>(a[0]); if (a.size() == 2) return put(in, *m, in.copy_value(a[1]), true, false);
        return put(in, *m, in.copy_value(a[1]), in.copy_value(a[2]), false);
    });
    M["map"]["try_emplace"] = M["map"]["emplace"];
    meth("map", "insert_or_assign", 3, 3, [put](Interp& in, Args& a) -> Value { return put(in, *as<Map>(a[0]), in.copy_value(a[1]), in.copy_value(a[2]), true); });
    meth("map", "erase", 2, 2, [](Interp& in, Args& a) -> Value {
        auto* m = as<Map>(a[0]); mutable_(in, *m);
        Value k = a[1]; if (auto* p = as<PosIt>(a[1])) { if (p->i < 0 || std::size_t(p->i) >= m->entries.size()) in.raise("IndexError", "erase(end())"); k = m->entries[std::size_t(p->i)].first; }
        bool had = m->find(k) != nullptr;
        if (had) {   // erase and keep order
            std::vector<std::pair<Value, Value>> es; for (auto& e : m->entries) if (!in.equal(e.first, k)) es.push_back(e);
            m->entries.clear(); m->index.clear(); for (auto& [x, y] : es) m->set(x, y);
        }
        return std::int64_t(had);
    });
    meth("map", "clear", 1, 1, [](Interp& in, Args& a) -> Value { auto* m = as<Map>(a[0]); mutable_(in, *m); m->entries.clear(); m->index.clear(); return Nil{}; });
    meth("map", "find", 2, 2, [](Interp& in, Args& a) -> Value {
        auto* m = as<Map>(a[0]);
        for (std::size_t k = 0; k < m->entries.size(); ++k) if (in.equal(m->entries[k].first, a[1])) return it(a[0], std::int64_t(k));
        return it(a[0], std::int64_t(m->entries.size()));
    });
    meth("map", "begin", 1, 1, [](Interp&, Args& a) -> Value { return it(a[0], 0); });
    meth("map", "end", 1, 1, [](Interp&, Args& a) -> Value { return it(a[0], clen(a[0])); });

    // std::optional / smart pointers / everything else
    meth("any", "has_value", 1, 1, [](Interp&, Args& a) -> Value { return !is_nil(a[0]); });
    meth("any", "value", 1, 1, [](Interp& in, Args& a) -> Value { if (is_nil(a[0])) in.raise("ValueError", "bad_optional_access: optional has no value"); return a[0]; });
    meth("any", "value_or", 2, 2, [](Interp&, Args& a) -> Value { return is_nil(a[0]) ? a[1] : a[0]; });
    meth("any", "get", 1, 1, [](Interp&, Args& a) -> Value { return a[0]; });
    meth("any", "use_count", 1, 1, [](Interp&, Args& a) -> Value { auto* r = std::get_if<Ref<Obj>>(&a[0]); return r && *r ? std::int64_t((*r)->rc.load()) - 1 : std::int64_t(0); });
    meth("weak", "lock", 1, 1, [](Interp&, Args& a) -> Value { return as<WeakRef>(a[0])->get(); });
    meth("weak", "expired", 1, 1, [](Interp&, Args& a) -> Value { return is_nil(as<WeakRef>(a[0])->get()); });
    meth("task", "get", 1, 1, [](Interp& in, Args& a) -> Value { return in.await(a[0]); });
    meth("task", "join", 1, 1, [](Interp& in, Args& a) -> Value { in.await(a[0]); return Nil{}; });
    meth("shared", "with", 2, 2, [](Interp& in, Args& a) -> Value {   // s.with([](auto& v) { ... }): runs under the lock
        auto* b = as<SharedBox>(a[0]);
        debug::lock_acquire(in, b->id);
        struct Rel { std::uint64_t id; ~Rel() { debug::lock_release(id); } } rel{b->id};
        std::unique_lock g(b->m, std::defer_lock);
        if (debug::on()) { debug::Blocked w(in, "forge::shared::with"); while (!g.try_lock_for(std::chrono::milliseconds(20))) { w.poll(); in.step(); } }
        else g.lock();
        Args x{b->v}; Value r = in.call_ref(a[1], x); b->v = x[0]; return r;
    });
    meth("shared", "load", 1, 1, [](Interp& in, Args& a) -> Value { auto* b = as<SharedBox>(a[0]); std::lock_guard g(b->m); return in.copy_value(b->v); });
    meth("shared", "store", 2, 2, [](Interp& in, Args& a) -> Value { auto* b = as<SharedBox>(a[0]); std::lock_guard g(b->m); b->v = in.copy_value(a[1]); return Nil{}; });
    if (auto c = sh.classes.find("ProcResult"); c != sh.classes.end()) {
        auto& ms = c->second->methods;
        if (ms.count("get_lines")) ms["lines"] = ms["get_lines"];
        if (ms.count("get_json")) ms["json"] = ms["get_json"];
    }
    // Borrow rule: a container that is being iterated by a range-for cannot be structurally modified.
    static const std::map<std::string, std::vector<std::string>> mut = {
        {"list", {"push_back", "emplace_back", "pop_back", "resize", "insert", "erase", "swap", "assign", "clear", "push", "pop", "extend", "remove", "append"}},
        {"map", {"insert", "emplace", "try_emplace", "insert_or_assign", "erase", "clear", "remove", "set", "swap"}},
        {"string", {"push_back", "append", "pop_back", "clear", "erase", "insert", "replace", "resize", "swap"}}};
    for (auto& [ty, names] : mut) {
        auto t = M.find(ty); if (t == M.end()) continue;
        for (auto& nm : names) {
            auto f = t->second.find(nm); if (f == t->second.end()) continue;
            auto* nat = as<Native>(f->second); if (!nat) continue;
            auto inner = nat->fn; std::string what = ty == "list" ? "vector" : ty;
            nat->fn = [inner, what, nm](Interp& in, Args& a) -> Value {
                if (auto* r = std::get_if<Ref<Obj>>(&a[0]); r && *r) {
                    if ((*r)->borrows.load() > 0) in.raise("BorrowError", "cannot call " + nm + "() on a " + what + " while it is being iterated");
                    debug::check_write(in, r->get());
                }
                return inner(in, a);
            };
        }
    }
}

} // namespace forge::script
