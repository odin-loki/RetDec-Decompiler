// forge::script runtime reflection (C++26 <meta> in spirit, but values not consteval info objects):
//   forge::typeof(v)                 -> type name ("int", "std::string"-style script names, class name)
//   forge::type_info(v | T | "T")    -> map describing the type (kind, name, fields, methods, bases, attrs, ...)
//   forge::reflect::{fields, methods, bases, attributes, get, set, has, invoke, call, enum_values,
//                    enum_name, symbols, signature, construct, find_type}
// Everything runs on the calling interpreter thread (tree-walker or VM: both call these natives through
// Interp::call), allocates through the script heap (charged to forge::mem host_budget "script"), and goes
// through the same checks as ordinary code: set() honours frozen objects, const and private members,
// declared field types and active borrows; invoke() refuses private and (on frozen objects) non-const
// methods; mutating operations need the `reflect` capability (sandbox({reflect: false}) turns them off).
#include "interp.hpp"
#include "ast.hpp"
#include "objects.hpp"
#include <algorithm>
#include <set>

namespace forge::script {
namespace {
using Args = std::vector<Value>;
using Fn = Native::fn_t;
Value strv(Interp& in, std::string s) { return in.new_string(std::move(s)); }
Value nat(const std::string& name, int lo, int hi, Fn f) { auto n = make<Native>(name, std::move(f), lo, hi); n->frozen = true; return obj(n); }
std::string s_of(Interp& in, const Value& v, const char* what) { auto* s = as<String>(v); if (!s) in.raise("TypeError", std::string(what) + " expects a string, got " + Interp::type_of(v)); return s->s; }
void need_reflect(Interp& in, const char* what) { if (!in.caps.reflect) in.raise("PermissionError", std::string("reflection (") + what + ") is disabled in this sandbox"); }
Value list_strs(Interp& in, const std::vector<std::string>& v) { auto l = make<List>(); for (auto& s : v) l->push(strv(in, s)); return obj(l); }

// Resolve a type/function/module by name the way code would: globals (incl. namespaces), then built-ins.
Value by_name(Interp& in, const std::string& n) {
    if (auto it = in.globals->vars.find(n); it != in.globals->vars.end()) return it->second;
    if (auto it = in.sh->builtins.find(n); it != in.sh->builtins.end()) return it->second;
    try { return in.load_qualified(n.find("::") == std::string::npos ? "::" + n : n); } catch (const ScriptError&) {}
    in.raise("NameError", "'" + n + "' is not defined");
}
// A class from an instance, a class value or a type name; nullptr for non-class values.
Class* class_of(Interp& in, const Value& v) {
    if (auto* c = as<Class>(v)) return c;
    if (auto* i = as<Instance>(v)) return i->cls.get();
    if (auto* s = as<String>(v)) return as<Class>(by_name(in, s->s));
    return nullptr;
}
Class& need_class(Interp& in, const Value& v, const char* what) {
    auto* c = class_of(in, v); if (!c) in.raise("TypeError", std::string(what) + " expects a class, struct, object or type name, got " + Interp::type_of(v));
    return *c;
}
const MemberMeta* field_meta(const Class& c, const std::string& n, const Class** owner = nullptr) {
    for (const Class* k = &c; k; k = k->base.get()) if (k->meta) if (auto* m = k->meta->field(n)) { if (owner) *owner = k; return m; }
    return nullptr;
}
const FnDecl* method_decl(const Class& c, const std::string& n) {
    if (auto* m = c.find_method(n)) if (auto* f = as<Function>(*m)) return f->decl.get();
    return nullptr;
}
Value field_map(Interp& in, const Class& owner, const std::string& name, const MemberMeta* m) {
    auto r = make<Map>();
    r->set_str("name", strv(in, name)); r->set_str("type", strv(in, m ? m->type : "auto")); r->set_str("owner", strv(in, owner.name));
    r->set_str("const", m && m->is_const); r->set_str("static", m && m->is_static); r->set_str("access", strv(in, m ? m->access : "public"));
    r->set_str("attributes", list_strs(in, m ? m->attrs : std::vector<std::string>{}));
    return obj(r);
}
Value sig_of_decl(Interp& in, const FnDecl& d, const std::string& kind) {
    auto r = make<Map>();
    auto nm = d.name; if (auto p = nm.rfind("::"); p != std::string::npos && kind != "function") nm = nm.substr(p + 2);
    if (nm == "init" || nm.rfind("init/", 0) == 0) nm = "constructor";
    r->set_str("name", strv(in, nm)); r->set_str("kind", strv(in, kind));
    r->set_str("returns", strv(in, d.ret_name.empty() ? "auto" : d.ret_name));
    auto ps = make<List>(); int required = 0; bool variadic = false;
    for (auto& p : d.params) {
        if (p.name == "__va") { variadic = true; continue; }
        auto pm = make<Map>(); pm->set_str("name", strv(in, p.name)); pm->set_str("type", strv(in, p.tname.empty() ? "auto" : p.tname));
        pm->set_str("default", bool(p.dflt)); pm->set_str("ref", p.by_ref); pm->set_str("out", p.out); pm->set_str("const", p.cnst);
        if (!p.dflt) ++required;
        ps->push(obj(pm));
    }
    r->set_str("params", obj(ps)); r->set_str("arity", std::int64_t(required)); r->set_str("variadic", variadic);
    r->set_str("const", d.is_const); r->set_str("static", d.is_static); r->set_str("virtual", d.is_virtual); r->set_str("access", strv(in, d.access));
    r->set_str("attributes", list_strs(in, d.attrs)); r->set_str("file", strv(in, d.file)); r->set_str("line", std::int64_t(d.line));
    return obj(r);
}
Value signature(Interp& in, const Value& f) {
    if (auto* fn = as<Function>(f)) return sig_of_decl(in, *fn->decl, fn->decl->is_method ? "method" : fn->owner ? "static" : "function");
    if (auto* b = as<Bound>(f)) return signature(in, b->fn);
    if (auto* n = as<Native>(f)) {
        auto r = make<Map>(); r->set_str("name", strv(in, n->name)); r->set_str("kind", strv(in, "native"));
        r->set_str("arity", std::int64_t(n->min_args)); r->set_str("max_args", std::int64_t(n->max_args)); r->set_str("variadic", n->max_args < 0);
        r->set_str("params", obj(make<List>())); r->set_str("returns", strv(in, "auto")); r->set_str("attributes", obj(make<List>()));
        return obj(r);
    }
    if (auto* c = as<Class>(f)) {   // the constructors
        auto l = make<List>();
        for (auto& [k, v] : c->methods) if (k == "init" || k.rfind("init/", 0) == 0) if (auto* fn = as<Function>(v)) l->push(sig_of_decl(in, *fn->decl, "constructor"));
        return obj(l);
    }
    in.raise("TypeError", "signature expects a function, method or class, got " + Interp::type_of(f));
}
// Fields in declaration order, base classes first (C++ layout order).
std::vector<std::pair<const Class*, std::string>> all_fields(const Class& c, bool statics) {
    std::vector<const Class*> chain; for (const Class* k = &c; k; k = k->base.get()) chain.push_back(k);
    std::vector<std::pair<const Class*, std::string>> out;
    for (auto it = chain.rbegin(); it != chain.rend(); ++it) {
        if ((*it)->meta) { for (auto& m : (*it)->meta->fields) if (m.is_static == statics) out.emplace_back(*it, m.name); }
        else if (!statics) for (auto& f : (*it)->fields) out.emplace_back(*it, f);
    }
    return out;
}
Value fields_of(Interp& in, const Class& c, bool statics) {
    auto l = make<List>();
    for (auto& [owner, n] : all_fields(c, statics)) l->push(field_map(in, *owner, n, owner->meta ? owner->meta->field(n) : nullptr));
    return obj(l);
}
Value methods_of(Interp& in, const Class& c) {
    auto l = make<List>(); std::set<std::string> seen;
    for (const Class* k = &c; k; k = k->base.get()) {
        std::vector<std::string> names; for (auto& [n, v] : k->methods) names.push_back(n);
        std::sort(names.begin(), names.end());
        for (auto& n : names) {
            if (n == "~" || n == "init" || n.rfind("init/", 0) == 0 || !seen.insert(n).second) continue;   // overridden names: most-derived only
            auto* fn = as<Function>(k->methods.at(n)); if (!fn) continue;
            auto m = sig_of_decl(in, *fn->decl, "method"); as<Map>(m)->set_str("owner", strv(in, k->name)); l->push(m);
        }
        for (auto& n : k->required) if (seen.insert(n).second) { auto m = make<Map>(); m->set_str("name", strv(in, n)); m->set_str("kind", strv(in, "pure_virtual")); m->set_str("owner", strv(in, k->name)); l->push(obj(m)); }
    }
    return obj(l);
}
Value bases_of(Interp& in, const Class& c) {
    auto l = make<List>();
    if (c.base) l->push(strv(in, c.base->name));
    for (auto& i : c.interfaces) l->push(strv(in, i));
    return obj(l);
}
std::string kind_of(const Value& v) {
    switch (v.index()) { case 0: return "nil"; case 1: return "bool"; case 2: return "int"; case 3: return "float"; }
    if (auto* m = as<Map>(v); m && !m->enum_name.empty()) return "enum";
    if (auto* c = as<Class>(v)) return c->is_interface ? "interface" : c->is_struct ? "struct" : "class";
    if (as<Instance>(v)) return "object";
    if (as<Function>(v) || as<Native>(v) || as<Bound>(v)) return "function";
    if (as<Globals>(v)) return "module";
    return std::get<Ref<Obj>>(v) ? std::get<Ref<Obj>>(v)->type_name() : "nil";
}
Value enum_values(Interp& in, const Map& m) {
    auto l = make<List>();
    for (auto& [k, v] : m.entries) { auto e = make<Map>(); e->set_str("name", k); e->set_str("value", v); l->push(obj(e)); }
    return obj(l);
}
Value type_info(Interp& in, const Value& v) {
    auto r = make<Map>();
    if (auto* m = as<Map>(v); m && !m->enum_name.empty()) {
        r->set_str("kind", strv(in, "enum")); r->set_str("name", strv(in, m->enum_name)); r->set_str("values", enum_values(in, *m)); return obj(r);
    }
    if (auto* c = class_of(in, v)) {
        r->set_str("kind", strv(in, c->is_interface ? "interface" : c->is_struct ? "struct" : "class")); r->set_str("name", strv(in, c->name));
        r->set_str("bases", bases_of(in, *c)); r->set_str("fields", fields_of(in, *c, false)); r->set_str("statics", fields_of(in, *c, true));
        r->set_str("methods", methods_of(in, *c)); r->set_str("constructors", signature(in, obj(Ref<Class>(c))));
        r->set_str("attributes", list_strs(in, c->meta ? c->meta->attrs : std::vector<std::string>{}));
        r->set_str("abstract", [&] { for (const Class* k = c; k; k = k->base.get()) for (auto& q : k->required) if (!c->find_method(q)) return true; return false; }());
        r->set_str("dynamic", c->is_dynamic);
        if (c->meta) { r->set_str("file", strv(in, c->meta->file)); r->set_str("line", std::int64_t(c->meta->line)); }
        if (auto* i = as<Instance>(v)) r->set_str("frozen", i->frozen);
        return obj(r);
    }
    r->set_str("kind", strv(in, kind_of(v))); r->set_str("name", strv(in, Interp::type_of(v)));
    if (as<Function>(v) || as<Native>(v) || as<Bound>(v)) r->set_str("signature", signature(in, v));
    if (auto* r0 = std::get_if<Ref<Obj>>(&v); r0 && *r0) r->set_str("frozen", (*r0)->frozen);
    return obj(r);
}
Value symbols(Interp& in, const Globals& g) {
    std::vector<std::string> names; for (auto& [k, v] : g.vars) if (!k.starts_with("__")) names.push_back(k);
    std::sort(names.begin(), names.end());
    auto l = make<List>();
    for (auto& n : names) {
        auto& v = g.vars.at(n); auto e = make<Map>();
        e->set_str("name", strv(in, n)); e->set_str("kind", strv(in, kind_of(v))); e->set_str("const", g.consts.count(n) > 0);
        l->push(obj(e));
    }
    return obj(l);
}
bool borrowed(const Value& v) { auto* r = std::get_if<Ref<Obj>>(&v); return r && *r && (*r)->borrows.load() > 0; }

Value get(Interp& in, const Value& o, const std::string& n) {
    if (auto* i = as<Instance>(o)) {
        const Class* owner = nullptr;
        if (auto* m = field_meta(*i->cls, n, &owner); m && m->access != "public") in.raise("AccessError", i->cls->name + "::" + n + " is " + m->access);
        if (!i->cls->has_field(n) && !i->fields.count(n) && !field_meta(*i->cls, n)) in.raise("AttributeError", i->cls->name + " has no field '" + n + "'");
    }
    if (auto* c = as<Class>(o)) if (auto* m = field_meta(*c, n); m && m->access != "public") in.raise("AccessError", c->name + "::" + n + " is " + m->access);
    return in.get_field(o, n);
}
void set(Interp& in, const Value& o, const std::string& n, Value v) {
    need_reflect(in, "set");
    auto check_meta = [&](const Class& c, bool is_static) {
        const MemberMeta* m = field_meta(c, n);
        if (!m && is_static) in.raise("AttributeError", c.name + " has no static member '" + n + "'");
        if (!m) return;
        if (m->is_static != is_static) in.raise("AttributeError", c.name + "::" + n + (m->is_static ? " is static; set it on the class" : " is not static"));
        if (m->access != "public") in.raise("AccessError", c.name + "::" + n + " is " + m->access);
        if (m->is_const) in.raise("TypeError", "cannot assign to const member " + c.name + "::" + n);
        if (m->ann) in.check_type(v, *m->ann, c.name + "::" + n);
    };
    if (auto* i = as<Instance>(o)) {
        if (i->frozen) in.raise("TypeError", "object is frozen");
        if (i->borrows.load() > 0) in.raise("BorrowError", "cannot set " + i->cls->name + "::" + n + " while the object is borrowed");
        check_meta(*i->cls, false);
        if (auto it = i->fields.find(n); it != i->fields.end() && borrowed(it->second)) in.raise("BorrowError", "cannot replace " + i->cls->name + "::" + n + " while it is being iterated");
        in.set_field(o, n, std::move(v)); return;
    }
    if (auto* c = as<Class>(o)) {
        check_meta(*c, true);
        if (c->frozen) in.raise("TypeError", "class is frozen");
        in.set_field(o, n, std::move(v)); return;
    }
    if (auto* m = as<Map>(o)) {
        if (!m->enum_name.empty()) in.raise("TypeError", "enum " + m->enum_name + " is immutable");
        if (m->borrows.load() > 0) in.raise("BorrowError", "cannot insert into a map while it is being iterated");
        in.set_field(o, n, std::move(v)); return;
    }
    in.raise("TypeError", "reflect::set expects an object, class or map, got " + Interp::type_of(o));
}
Value invoke(Interp& in, const Value& o, const std::string& n, Args args) {
    need_reflect(in, "invoke");
    if (auto* i = as<Instance>(o)) {
        if (auto* d = method_decl(*i->cls, n)) {
            if (d->access != "public") in.raise("AccessError", i->cls->name + "::" + n + "() is " + d->access);
            if (i->frozen && !d->is_const) in.raise("TypeError", i->cls->name + "::" + n + "() is not const and the object is frozen");
        } else if (!i->cls->find_method(n)) in.raise("AttributeError", i->cls->name + " has no method '" + n + "'");
    }
    if (auto* c = as<Class>(o)) if (auto* d = method_decl(*c, n); d && d->access != "public") in.raise("AccessError", c->name + "::" + n + "() is " + d->access);
    Value f = in.get_field(o, n);
    return in.call(f, args);
}
} // namespace

void install_reflect(Shared& sh) {
    auto& B = sh.builtins;
    B["forge::__enum"] = nat("forge::__enum", 2, 2, [](Interp& in, Args& a) -> Value {
        auto* m = as<Map>(a[1]); if (!m) in.raise("TypeError", "__enum expects a map");
        m->enum_name = s_of(in, a[0], "__enum"); m->frozen = true; return a[1];
    });
    B["forge::typeof"] = nat("forge::typeof", 1, 1, [](Interp&, Args& a) -> Value { return obj(make<String>(Interp::type_of(a[0]))); });
    B["forge::type_info"] = nat("forge::type_info", 1, 1, [](Interp& in, Args& a) -> Value { return type_info(in, a[0]); });
    const std::string R = "forge::reflect::";
    auto def = [&](const std::string& n, int lo, int hi, Fn f) { B[R + n] = nat(R + n, lo, hi, std::move(f)); };
    def("type_info", 1, 1, [](Interp& in, Args& a) -> Value { return type_info(in, a[0]); });
    def("typeof", 1, 1, [](Interp&, Args& a) -> Value { return obj(make<String>(Interp::type_of(a[0]))); });
    def("kind", 1, 1, [](Interp& in, Args& a) -> Value { return strv(in, kind_of(a[0])); });
    def("find_type", 1, 1, [](Interp& in, Args& a) -> Value { Value v = by_name(in, s_of(in, a[0], "reflect::find_type")); if (!as<Class>(v) && !(as<Map>(v) && !as<Map>(v)->enum_name.empty())) in.raise("TypeError", in.str(a[0]) + " is not a type"); return v; });
    def("fields", 1, 1, [](Interp& in, Args& a) -> Value { return fields_of(in, need_class(in, a[0], "reflect::fields"), false); });
    def("statics", 1, 1, [](Interp& in, Args& a) -> Value { return fields_of(in, need_class(in, a[0], "reflect::statics"), true); });
    def("methods", 1, 1, [](Interp& in, Args& a) -> Value { return methods_of(in, need_class(in, a[0], "reflect::methods")); });
    def("bases", 1, 1, [](Interp& in, Args& a) -> Value { return bases_of(in, need_class(in, a[0], "reflect::bases")); });
    def("is_base_of", 2, 2, [](Interp& in, Args& a) -> Value { return need_class(in, a[1], "reflect::is_base_of").derives(need_class(in, a[0], "reflect::is_base_of").name); });
    def("attributes", 1, 2, [](Interp& in, Args& a) -> Value {
        if (auto* f = as<Function>(a[0]); f && a.size() == 1) return list_strs(in, f->decl->attrs);
        if (auto* b = as<Bound>(a[0]); b && a.size() == 1) if (auto* f = as<Function>(b->fn)) return list_strs(in, f->decl->attrs);
        auto& c = need_class(in, a[0], "reflect::attributes");
        if (a.size() == 1) return list_strs(in, c.meta ? c.meta->attrs : std::vector<std::string>{});
        auto n = s_of(in, a[1], "reflect::attributes member");
        if (auto* m = field_meta(c, n)) return list_strs(in, m->attrs);
        if (auto* d = method_decl(c, n)) return list_strs(in, d->attrs);
        in.raise("AttributeError", c.name + " has no member '" + n + "'");
    });
    def("has", 2, 2, [](Interp& in, Args& a) -> Value {
        auto* c = class_of(in, a[0]); auto n = s_of(in, a[1], "reflect::has");
        if (c) return c->has_field(n) || field_meta(*c, n) || c->find_method(n) != nullptr || (as<Instance>(a[0]) && as<Instance>(a[0])->fields.count(n));
        if (auto* m = as<Map>(a[0])) return m->find_str(n) != nullptr;
        if (auto* g = as<Globals>(a[0])) return g->vars.count(n) > 0;
        return false;
    });
    def("get", 2, 2, [](Interp& in, Args& a) -> Value { return get(in, a[0], s_of(in, a[1], "reflect::get")); });
    def("set", 3, 3, [](Interp& in, Args& a) -> Value { set(in, a[0], s_of(in, a[1], "reflect::set"), a[2]); return Nil{}; });
    def("invoke", 2, -1, [](Interp& in, Args& a) -> Value { Args rest(a.begin() + 2, a.end()); return invoke(in, a[0], s_of(in, a[1], "reflect::invoke"), std::move(rest)); });
    def("call", 1, -1, [](Interp& in, Args& a) -> Value {   // a free function / built-in by (qualified) name
        need_reflect(in, "call");
        Value f = by_name(in, s_of(in, a[0], "reflect::call"));
        if (!as<Function>(f) && !as<Native>(f) && !as<Bound>(f)) in.raise("TypeError", in.str(a[0]) + " is not a function");
        Args rest(a.begin() + 1, a.end()); return in.call(f, rest);
    });
    def("construct", 1, -1, [](Interp& in, Args& a) -> Value {
        need_reflect(in, "construct");
        auto* c = as<String>(a[0]) ? as<Class>(by_name(in, as<String>(a[0])->s)) : as<Class>(a[0]);
        if (!c) in.raise("TypeError", in.str(a[0]) + " is not a class or struct");
        Args rest(a.begin() + 1, a.end()); return in.instantiate(Ref<Class>(c), rest);
    });
    def("enum_values", 1, 1, [](Interp& in, Args& a) -> Value {
        Value v = as<String>(a[0]) ? by_name(in, as<String>(a[0])->s) : a[0];
        auto* m = as<Map>(v); if (!m || m->enum_name.empty()) in.raise("TypeError", "enum_values expects an enum, got " + Interp::type_of(v));
        return enum_values(in, *m);
    });
    def("enum_name", 2, 2, [](Interp& in, Args& a) -> Value {   // value -> enumerator name (nil if none)
        auto* m = as<Map>(a[0]); if (!m || m->enum_name.empty()) in.raise("TypeError", "enum_name expects an enum");
        for (auto& [k, v] : m->entries) if (in.equal(v, a[1])) return k;
        return Nil{};
    });
    def("symbols", 0, 1, [](Interp& in, Args& a) -> Value {
        if (a.empty() || is_nil(a[0])) return symbols(in, *in.globals);
        if (auto* g = as<Globals>(a[0])) return symbols(in, *g);
        if (auto* m = as<Map>(a[0])) {   // built-in library (fs, os, ...) or namespace map
            auto l = make<List>();
            for (auto& [k, v] : m->entries) { auto e = make<Map>(); e->set_str("name", k); e->set_str("kind", strv(in, kind_of(v))); e->set_str("const", m->frozen); l->push(obj(e)); }
            return obj(l);
        }
        in.raise("TypeError", "symbols expects a module, got " + Interp::type_of(a[0]));
    });
    def("signature", 1, 1, [](Interp& in, Args& a) -> Value { Value f = as<String>(a[0]) ? by_name(in, as<String>(a[0])->s) : a[0]; return signature(in, f); });
}
} // namespace forge::script
