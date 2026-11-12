// forge::script AST tree-walker: the REPL / eval path, and delegated statements from the VM.
#include "interp.hpp"
#include <exception>

namespace forge::script {

Flow Interp::exec_block(const Node& n, const Ref<Env>& env) {
    std::vector<const Node*> defers;
    auto run_defers = [&]() {
        for (auto it = defers.rbegin(); it != defers.rend(); ++it) {
            Value saved = ret;
            exec_block(*(*it)->kids[0], env);
            ret = saved;
        }
    };
    struct Scope {   // RAII: destroy block locals in reverse declaration order on every exit path
        Interp& in; const Node& n; const Ref<Env>& env;
        ~Scope() {
            if (n.locals.empty() || !env) return;
            for (auto it = n.locals.rbegin(); it != n.locals.rend(); ++it)
                if (std::size_t(*it) < env->slots.size()) { Value dead = std::move(env->slots[std::size_t(*it)]); env->slots[std::size_t(*it)] = Nil{}; }
        }
    } scope{*this, n, env};
    // Defers run from a destructor while an error unwinds, not from catch (...) { ...; throw; }: MSVC keeps every
    // frame alive until a catch block exits, so one rethrow per nested block overflowed the stack on deep recursion.
    struct Unwind {
        std::vector<const Node*>& defers; decltype(run_defers)& run; int pending = std::uncaught_exceptions(); bool done = false;
        ~Unwind() { if (!done && !defers.empty() && std::uncaught_exceptions() > pending) { try { run(); } catch (...) {} } }
    } unwind{defers, run_defers};
    Flow f = Flow::Normal;
    for (auto& s : n.kids) {
        if (s->k == NK::Defer) { defers.push_back(s.get()); continue; }
        f = exec(*s, env);
        if (f != Flow::Normal) break;
    }
    unwind.done = true;
    run_defers();
    return f;
}

Flow Interp::run_try(const Node& n, const Ref<Env>& env) {
    Flow f = Flow::Normal;
    std::exception_ptr pending;
    try { f = exec_block(*n.kids[0], env); }
    catch (...) {
        ScriptError e = from_cpp(std::current_exception());
        const Node* handler = nullptr;
        for (std::size_t i = 1; i < n.kids.size(); ++i) {
            const Node& c = *n.kids[i];
            if (c.s.empty() || c.s == "Error") { handler = &c; break; }
            if (auto* inst = as<Instance>(e.v); inst && inst->cls->derives(c.s)) { handler = &c; break; }
            if (type_of(e.v) == c.s) { handler = &c; break; }
        }
        if (handler) {
            store(handler->var, env, e.v);
            try { f = exec_block(*handler->kids[0], env); }
            catch (...) { pending = std::current_exception(); }
        } else pending = std::make_exception_ptr(e);
    }
    if (n.extra) {
        Value saved = ret;
        Flow ff = exec_block(*n.extra, env);
        if (ff != Flow::Normal) return ff;   // finally's own break/return wins (and drops the error)
        ret = saved;
    }
    if (pending) std::rethrow_exception(pending);
    return f;
}

Flow Interp::run_using(const Node& n, const Ref<Env>& env) {
    Value v = eval(*n.kids[0], env);
    store(n.var, env, v);
    Flow f;
    try { f = exec_block(*n.kids[1], env); }
    catch (...) { try { dispose(v); } catch (...) {} throw; }
    dispose(v);
    return f;
}

Flow Interp::exec(const Node& n, const Ref<Env>& env) {
    line = n.line;
    step();
    switch (n.k) {
    case NK::ExprStmt: eval(*n.kids[0], env); return Flow::Normal;
    case NK::Let: {
        Value v = n.kids.empty() ? Value(Nil{}) : eval(*n.kids[0], env);
        if (n.type) check_type(v, *n.type, "let " + n.s);
        store(n.var, env, std::move(v), true, n.flag);
        return Flow::Normal;
    }
    case NK::Assign: {
        const Node& t = *n.kids[0];
        Op op = Op(n.i);
        if (t.k == NK::Var) {
            Value v = eval(*n.kids[1], env);
            if (op != Op::None) v = binary(op, load(t.var, env), v);
            store(t.var, env, std::move(v));
        } else if (t.k == NK::Field) {
            Value o = eval(*t.kids[0], env);
            Value v = eval(*n.kids[1], env);
            if (op != Op::None) v = binary(op, get_field(o, t.s), v);
            set_field(o, t.s, std::move(v));
        } else {
            Value o = eval(*t.kids[0], env), i = eval(*t.kids[1], env);
            Value v = eval(*n.kids[1], env);
            if (op != Op::None) v = binary(op, get_index(o, i), v);
            set_index(o, i, std::move(v));
        }
        return Flow::Normal;
    }
    case NK::If:
        if (truthy(eval(*n.kids[0], env))) return exec_block(*n.kids[1], env);
        if (n.kids.size() > 2) return exec_block(*n.kids[2], env);
        return Flow::Normal;
    case NK::While:
        while (truthy(eval(*n.kids[0], env))) {
            step();
            Flow f = exec_block(*n.kids[1], env);
            if (f == Flow::Break) break;
            if (f == Flow::Return) return f;
            if (n.extra) {
                Flow g = exec_block(*n.extra, env);
                if (g == Flow::Break) break;
                if (g == Flow::Return) return g;
            }
        }
        return Flow::Normal;
    case NK::For: {
        Value src = eval(*n.kids[0], env);
        auto it = iterate(src);
        Value a, b;
        VarRef second{"", 0, int(n.i)};
        while (iter_next(*it, a, n.flag ? &b : nullptr)) {
            store(n.var, env, a);
            if (n.flag) store(second, env, b);
            if (n.s == "ref") {   // for (T& x : c): assignments to x write through to the element
                // Only a changed binding is written back, so writes made through the container itself
                // (m[k] = x + 1) are not overwritten by the stale reference value.
                auto same = [&](const Value& x, const Value& y) { auto* p = std::get_if<Ref<Obj>>(&x); auto* q = std::get_if<Ref<Obj>>(&y); return (p && q) ? p->get() == q->get() : (!p && !q && x == y); };
                Value a0 = a, b0 = b;
                Flow f = exec_block(*n.kids[1], env);   // (block locals are cleared; the loop variables live in the outer block)
                Value a1 = load(n.var, env), b1 = n.flag ? load(second, env) : Value(Nil{});
                if (!same(a0, a1) || (n.flag && !same(b0, b1))) iter_writeback(*it, a1, n.flag ? &b1 : nullptr);
                if (f == Flow::Break) break;
                if (f == Flow::Return) return f;
                step();
                continue;
            }
            Flow f = exec_block(*n.kids[1], env);
            if (f == Flow::Break) break;
            if (f == Flow::Return) return f;
            step();
        }
        return Flow::Normal;
    }
    case NK::FnStmt: store(n.var, env, make_closure(n.fn, env), true); return Flow::Normal;
    case NK::Return: ret = n.kids.empty() ? Value(Nil{}) : eval(*n.kids[0], env); return Flow::Return;
    case NK::Break: return Flow::Break;
    case NK::Continue: return Flow::Continue;
    case NK::Throw: {
        Value v = eval(*n.kids[0], env);
        if (auto* s = as<String>(v)) v = make_error("Error", s->s);
        if (auto* i = as<Instance>(v); i && i->cls->derives("Error")) {
            if (auto it = i->fields.find("trace"); it == i->fields.end() || is_nil(it->second) || (as<String>(it->second) && as<String>(it->second)->s.empty()))
                i->fields["trace"] = new_string(trace_text());
            i->fields["line"] = std::int64_t(n.line);
        }
        throw ScriptError(v, error_message(v));
    }
    case NK::Try: return run_try(n, env);
    case NK::Using: return run_using(n, env);
    case NK::Defer: return Flow::Normal;   // handled by exec_block
    case NK::ClassStmt: store(n.var, env, define_class(*n.cls, env), true, true); return Flow::Normal;
    case NK::Import: {
        Value m = import_module(n.s);
        if (!n.flag) { store(n.var, env, m, true, true); return Flow::Normal; }
        if (auto* g = as<Globals>(m)) for (auto& [k, v] : g->vars) if (!globals->vars.count(k)) { globals->vars[k] = v; if (g->consts.count(k)) globals->consts.insert(k); }
        if (auto* pm = as<Map>(m)) for (auto& [k, v] : pm->entries) if (auto* ks = as<String>(k); ks && !globals->vars.count(ks->s)) { globals->vars[ks->s] = v; globals->consts.insert(ks->s); }
        return Flow::Normal;
    }
    case NK::Block: return exec_block(n, env);
    default: eval(n, env); return Flow::Normal;
    }
}

Value Interp::eval(const Node& n, const Ref<Env>& env) {
    switch (n.k) {
    case NK::Nil: return Nil{};
    case NK::True: return true;
    case NK::False: return false;
    case NK::Int: return n.i;
    case NK::Float: return n.d;
    case NK::Str: return new_string(n.s);
    case NK::Interp: { std::vector<Value> parts; for (auto& k : n.kids) parts.push_back(eval(*k, env)); return interpolate(parts); }
    case NK::Var: return load(n.var, env);
    case NK::List: { auto l = make<List>(); for (auto& k : n.kids) l->push(share(eval(*k, env))); return obj(l); }
    case NK::Map: {
        auto m = make<Map>();
        for (std::size_t i = 0; i + 1 < n.kids.size(); i += 2) {
            Value k = eval(*n.kids[i], env);
            if (!key_of(k)) raise("TypeError", "map keys must be nil, bool, numbers or strings");
            m->set(k, share(eval(*n.kids[i + 1], env)));
        }
        return obj(m);
    }
    case NK::Unary: return unary(n.s, eval(*n.kids[0], env));
    case NK::Binary: { Value a = eval(*n.kids[0], env); Value b = eval(*n.kids[1], env); line = n.line; return binary(Op(n.i), a, b); }
    case NK::And: return truthy(eval(*n.kids[0], env)) && truthy(eval(*n.kids[1], env));
    case NK::Or: return truthy(eval(*n.kids[0], env)) || truthy(eval(*n.kids[1], env));
    case NK::Ternary: return truthy(eval(*n.kids[0], env)) ? eval(*n.kids[1], env) : eval(*n.kids[2], env);
    case NK::Copy: return copy_value(eval(*n.kids[0], env), -1);
    case NK::Move: return move_from(*n.kids[0], env);
    case NK::Coalesce: { Value a = eval(*n.kids[0], env); return is_nil(a) ? eval(*n.kids[1], env) : a; }
    case NK::Call: {
        if (n.wb) return call_writeback(n, env);
        Value f = eval(*n.kids[0], env);
        std::vector<Value> args; args.reserve(n.kids.size() - 1);
        for (std::size_t i = 1; i < n.kids.size(); ++i) args.push_back(eval(*n.kids[i], env));
        return call(f, args, n.line);
    }
    case NK::Index: { Value o = eval(*n.kids[0], env); Value i = eval(*n.kids[1], env); line = n.line; return get_index(o, i); }
    case NK::Field: { Value o = eval(*n.kids[0], env); line = n.line; return get_field(o, n.s); }
    case NK::OptField: { Value o = eval(*n.kids[0], env); return get_field(o, n.s, true); }
    case NK::Lambda: return make_closure(n.fn, env);
    case NK::Shell: {
        const Node& b = *n.kids[0];
        std::vector<Value> parts;
        if (b.k == NK::Interp) for (auto& k : b.kids) parts.push_back(eval(*k, env));
        line = n.line;
        return shell(n, parts);
    }
    case NK::Is: return conforms(eval(*n.kids[0], env), *n.type);
    case NK::Spawn: {
        Value f = eval(*n.kids[0], env);
        std::vector<Value> args; for (std::size_t i = 1; i < n.kids.size(); ++i) args.push_back(eval(*n.kids[i], env));
        return spawn(f, std::move(args));
    }
    case NK::Await: return await(eval(*n.kids[0], env));
    case NK::Super: {
        Value self = load(n.var, env);
        auto* inst = as<Instance>(self);
        if (!inst) raise("TypeError", "super needs an object");
        const Class* owner = inst->cls.get();
        while (owner && owner->name != n.type->name) owner = owner->base.get();
        const Class* base = owner ? owner->base.get() : nullptr;
        if (!base) raise("AttributeError", n.type->name + " has no base class for super");
        auto* m = base->find_method(n.s);
        if (!m) raise("AttributeError", "base class has no method '" + n.s + "'");
        return obj(make<Bound>(self, *m));
    }
    default: raise("SyntaxError", "statement used as an expression");
    }
}

// std::move(lvalue): take the value and leave a moved-from marker; reading it later raises BorrowError.
// Scalars are trivially copyable, so moving one leaves the source untouched (as in C++).
Value Interp::move_from(const Node& t, const Ref<Env>& env) {
    Value v = eval(t, env);
    auto* r = std::get_if<Ref<Obj>>(&v);
    if (!r || !*r) return v;
    if (dynamic_cast<Class*>(r->get()) || dynamic_cast<Native*>(r->get())) return v;
    Value m = Value(Ref<Obj>(moved_sentinel()));
    if (t.k == NK::Var) {
        if (t.var.global()) { if (auto it = globals->vars.find(t.var.name); it != globals->vars.end()) it->second = m; }
        else { Env* e = env.get(); for (int d = t.var.depth; d > 0 && e; --d) e = e->parent.get(); if (e) e->slots.at(std::size_t(t.var.slot)) = m; }
    } else if (t.k == NK::Field) {
        Value o = eval(*t.kids[0], env);
        if (auto* i = as<Instance>(o)) i->fields[t.s] = m;
    } else if (t.k == NK::Index) {
        Value o = eval(*t.kids[0], env), k = eval(*t.kids[1], env);
        if (auto* l = as<List>(o)) { if (auto* ix = std::get_if<std::int64_t>(&k); ix && *ix >= 0 && std::size_t(*ix) < l->items.size()) l->items[std::size_t(*ix)] = m; }
        else if (auto* mp = as<Map>(o)) mp->set(k, m);
    }
    return v;
}

Value Interp::call_ref(const Value& f, std::vector<Value>& args) {
    const Function* target = nullptr;
    if (auto* fn = as<Function>(f)) target = fn; else if (auto* b = as<Bound>(f)) target = as<Function>(b->fn);
    std::vector<Value> keep = args;   // call() may consume the argument vector
    ref_back_fn = nullptr; ref_back.clear();
    Value r = call(f, keep);
    if (target && ref_back_fn == target) for (auto& [i, v] : ref_back) if (i < args.size()) args[i] = v;
    ref_back.clear(); ref_back_fn = nullptr;
    return r;
}
// f(x, v[i], o.m) where f takes `T&`: lvalue arguments are evaluated once (object and index cached) and
// receive the parameter's final value after the call, so `void inc(int& x) { ++x; }` works on scalars.
Value Interp::call_writeback(const Node& n, const Ref<Env>& env) {
    Value f = eval(*n.kids[0], env);
    struct LV { const Node* n; Value o, i; };
    std::vector<LV> lv; std::vector<Value> args;
    for (std::size_t k = 1; k < n.kids.size(); ++k) {
        const Node& a = *n.kids[k];
        if (a.k == NK::Field) { Value o = eval(*a.kids[0], env); line = a.line; args.push_back(get_field(o, a.s)); lv.push_back({&a, o, Nil{}}); }
        else if (a.k == NK::Index) { Value o = eval(*a.kids[0], env), i = eval(*a.kids[1], env); line = a.line; args.push_back(get_index(o, i)); lv.push_back({&a, o, i}); }
        else { args.push_back(eval(a, env)); lv.push_back({&a, Nil{}, Nil{}}); }
    }
    const Function* target = nullptr;
    if (auto* fn = as<Function>(f)) target = fn;
    else if (auto* b = as<Bound>(f)) target = as<Function>(b->fn);
    ref_back_fn = nullptr; ref_back.clear();
    Value r = call(f, args, n.line);
    if (!target || ref_back_fn != target) return r;
    auto back = std::move(ref_back); ref_back.clear(); ref_back_fn = nullptr;
    for (auto& [i, v] : back) {
        if (i >= lv.size()) continue;
        auto& l = lv[i];
        auto same = [&](const Value& cur) { auto* a = std::get_if<Ref<Obj>>(&cur); auto* b = std::get_if<Ref<Obj>>(&v); return a && b && a->get() == b->get(); };
        if (l.n->k == NK::Var) { if (!same(load(l.n->var, env))) store(l.n->var, env, v); }
        else if (l.n->k == NK::Field) { if (!same(get_field(l.o, l.n->s))) set_field(l.o, l.n->s, v); }
        else if (l.n->k == NK::Index) { if (!same(get_index(l.o, l.i))) set_index(l.o, l.i, v); }
    }
    return r;
}

} // namespace forge::script
