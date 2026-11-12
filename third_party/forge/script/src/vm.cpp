// forge::script bytecode compiler, verifier and VM. Hot code (expressions, calls, loops, branches)
// runs as compact stack bytecode; cold structured statements (try, using, defer blocks, class, import,
// throw, compound index assignment) are delegated to the tree-walker through ExecNode, which keeps one
// implementation of their unwinding rules.
#include "vm.hpp"
#include <format>
#include <stdexcept>

namespace forge::script {
namespace {

struct Loop { std::vector<int> brk_jumps, cont_jumps; std::vector<int> brk_exec, cont_exec; int cont = 0; bool is_for = false; };

class Compiler {
public:
    explicit Compiler(const FnDecl& fd) : fd_(fd) { c_.name = fd.name; c_.file = fd.file; c_.nslots = fd.nslots; }
    std::shared_ptr<const Chunk> run() {
        block_inline(*fd_.body);
        emit(Opc::ReturnNil, 0, 0, fd_.line);
        return std::make_shared<Chunk>(std::move(c_));
    }
private:
    int emit(Opc op, int a = 0, int b = 0, int line = 0) { c_.code.push_back({op, a, b}); c_.lines.push_back(line ? line : line_); return int(c_.code.size()) - 1; }
    int here() const { return int(c_.code.size()); }
    void patch(int at, int target) { c_.code.at(std::size_t(at)).a = target; }
    int name(const std::string& s) { for (std::size_t i = 0; i < c_.names.size(); ++i) if (c_.names[i] == s) return int(i); c_.names.push_back(s); return int(c_.names.size()) - 1; }
    int konst(Value v) { c_.consts.push_back(std::move(v)); return int(c_.consts.size()) - 1; }
    int node(const NodeP& n) { c_.nodes.push_back(n); return int(c_.nodes.size()) - 1; }
    NodeP keep(const Node& n) {   // find the shared_ptr for a node reachable from the function body
        return find_(fd_.body, &n);
    }
    static NodeP find_(const NodeP& root, const Node* target) {
        if (!root) return nullptr;
        if (root.get() == target) return root;
        for (auto& k : root->kids) if (auto r = find_(k, target)) return r;
        if (auto r = find_(root->extra, target)) return r;
        return nullptr;
    }
    static bool has_defer(const Node& b) { for (auto& s : b.kids) if (s->k == NK::Defer) return true; return false; }

    void exec_node(const Node& n) {
        int idx = node(keep(n));
        int t = int(exec_targets_.size()); exec_targets_.push_back({-1, -1});
        if (!loops_.empty()) { loops_.back().brk_exec.push_back(t); exec_targets_[std::size_t(t)].second = loops_.back().cont; if (loops_.back().cont < 0) loops_.back().cont_exec.push_back(t); }
        emit(Opc::ExecNode, idx, t, n.line);
    }
    void block_inline(const Node& b) {
        if (has_defer(b)) { exec_node(b); return; }
        for (auto& s : b.kids) stmt(*s);
        clear_locals(b.locals);
    }
    void clear_locals(const std::vector<int>& ls) {   // RAII: destroy block locals in reverse declaration order
        for (auto it = ls.rbegin(); it != ls.rend(); ++it) emit(Opc::ClearLocal, 0, *it);
    }
    void store(const VarRef& v, bool define, bool is_const, int line) {
        if (v.global()) emit(define ? (is_const ? Opc::DefineConst : Opc::DefineGlobal) : Opc::StoreGlobal, name(v.name), 0, line);
        else emit(Opc::StoreLocal, v.depth, v.slot, line);
    }
    void load(const VarRef& v, int line) {
        if (v.global()) emit(Opc::LoadGlobal, name(v.name), 0, line);
        else emit(Opc::LoadLocal, v.depth, v.slot, line);
    }
    void stmt(const Node& n) {
        line_ = n.line;
        switch (n.k) {
        case NK::ExprStmt: emit(Opc::Line, n.line); expr(*n.kids[0]); emit(Opc::Pop); return;
        case NK::Let:
            emit(Opc::Line, n.line);
            if (n.kids.empty()) emit(Opc::Nil); else expr(*n.kids[0]);
            if (n.type) emit(Opc::CheckType, node(keep(n)));
            store(n.var, true, n.flag, n.line);
            return;
        case NK::Assign: {
            const Node& t = *n.kids[0]; Op op = Op(n.i);
            emit(Opc::Line, n.line);
            if (t.k == NK::Var) {
                if (op != Op::None) load(t.var, n.line);
                expr(*n.kids[1]);
                if (op != Op::None) emit(Opc::Binary, int(op));
                store(t.var, false, false, n.line);
            } else if (t.k == NK::Field) {
                expr(*t.kids[0]);
                if (op != Op::None) { emit(Opc::Dup); emit(Opc::GetField, name(t.s)); }
                expr(*n.kids[1]);
                if (op != Op::None) emit(Opc::Binary, int(op));
                emit(Opc::SetField, name(t.s));
            } else if (op == Op::None) {
                expr(*t.kids[0]); expr(*t.kids[1]); expr(*n.kids[1]); emit(Opc::SetIndex);
            } else exec_node(n);
            return;
        }
        case NK::If: {
            emit(Opc::Line, n.line);
            expr(*n.kids[0]);
            int jf = emit(Opc::JumpIfFalse);
            block_inline(*n.kids[1]);
            if (n.kids.size() > 2) { int je = emit(Opc::Jump); patch(jf, here()); block_inline(*n.kids[2]); patch(je, here()); }
            else patch(jf, here());
            return;
        }
        case NK::While: {
            int start = here();
            emit(Opc::Line, n.line);
            expr(*n.kids[0]);
            int jf = emit(Opc::JumpIfFalse);
            loops_.push_back({}); loops_.back().cont = n.extra ? -1 : start;
            block_inline(*n.kids[1]);
            if (n.extra) {   // for(;;) step / do-while test: `continue` lands here
                int step = here();
                for (int j : loops_.back().cont_jumps) patch(j, step);
                for (int t : loops_.back().cont_exec) exec_targets_[std::size_t(t)].second = step;
                loops_.back().cont = step;
                block_inline(*n.extra);
            }
            emit(Opc::Jump, start);
            int end = here(); patch(jf, end);
            close_loop(end);
            return;
        }
        case NK::For: {
            if (n.s == "ref") { exec_node(n); return; }   // write-through element references: tree-walker
            emit(Opc::Line, n.line);
            expr(*n.kids[0]);
            emit(Opc::Iter);
            int next = here();
            int in = emit(n.flag ? Opc::IterNext2 : Opc::IterNext);
            if (n.flag) { emit(Opc::StoreLocal, 0, int(n.i)); }
            emit(Opc::StoreLocal, n.var.depth, n.var.slot);
            loops_.push_back({}); loops_.back().cont = next; loops_.back().is_for = true;
            block_inline(*n.kids[1]);
            emit(Opc::Jump, next);
            int brk = here(); emit(Opc::Pop);   // break: drop the iterator
            int end = here(); patch(in, end);
            close_loop(brk);
            return;
        }
        case NK::FnStmt: emit(Opc::Line, n.line); emit(Opc::Closure, node(keep(n))); store(n.var, true, false, n.line); return;
        case NK::Return:
            emit(Opc::Line, n.line);
            if (n.kids.empty()) emit(Opc::ReturnNil); else { expr(*n.kids[0]); emit(Opc::Return); }
            return;
        case NK::Break: loops_.back().brk_jumps.push_back(emit(Opc::Jump)); return;
        case NK::Continue: { int j = emit(Opc::Jump, loops_.back().cont); if (loops_.back().cont < 0) loops_.back().cont_jumps.push_back(j); return; }
        case NK::Block: block_inline(n); return;
        case NK::Defer: return;   // only reachable through a delegated block
        default: exec_node(n); return;   // Try, Using, ClassStmt, Import, Throw
        }
    }
    void close_loop(int brk_target) {
        auto lp = std::move(loops_.back()); loops_.pop_back();
        for (int j : lp.brk_jumps) patch(j, brk_target);
        for (int t : lp.brk_exec) exec_targets_[std::size_t(t)].first = brk_target;
    }
    void expr(const Node& n) {
        int line = n.line;
        switch (n.k) {
        case NK::Nil: emit(Opc::Nil); return;
        case NK::True: emit(Opc::True); return;
        case NK::False: emit(Opc::False); return;
        case NK::Int: emit(Opc::Const, konst(n.i)); return;
        case NK::Float: emit(Opc::Const, konst(n.d)); return;
        case NK::Str: { auto s = make<String>(n.s); s->frozen = true; emit(Opc::Const, konst(obj(s))); return; }
        case NK::Interp: for (auto& k : n.kids) expr(*k); emit(Opc::Interp, int(n.kids.size())); return;
        case NK::Var: load(n.var, line); return;
        case NK::List: for (auto& k : n.kids) expr(*k); emit(Opc::MakeList, int(n.kids.size())); return;
        case NK::Map: for (auto& k : n.kids) expr(*k); emit(Opc::MakeMap, int(n.kids.size() / 2)); return;
        case NK::Unary: expr(*n.kids[0]); emit(Opc::Unary, n.s == "not" ? 1 : n.s == "~" ? 3 : n.s == "bool" ? 2 : 0); return;
        case NK::Ternary: {
            expr(*n.kids[0]); int jf = emit(Opc::JumpIfFalse);
            expr(*n.kids[1]); int je = emit(Opc::Jump); patch(jf, here());
            expr(*n.kids[2]); patch(je, here()); return;
        }
        case NK::Copy: expr(*n.kids[0]); emit(Opc::Copy, 0, 0, line); return;
        case NK::Binary: expr(*n.kids[0]); expr(*n.kids[1]); emit(Opc::Binary, int(n.i), 0, line); return;
        case NK::And: case NK::Or: case NK::Coalesce: {
            expr(*n.kids[0]);
            int j = emit(n.k == NK::And ? Opc::JumpIfFalseKeep : n.k == NK::Or ? Opc::JumpIfTrueKeep : Opc::JumpIfNotNilKeep);
            expr(*n.kids[1]); patch(j, here());
            if (n.k != NK::Coalesce) emit(Opc::Unary, 2);   // && and || yield bool
            return;
        }
        case NK::Call: if (n.wb) { emit(Opc::EvalNode, node(keep(n)), 0, line); return; }
            for (auto& k : n.kids) expr(*k); emit(Opc::Call, int(n.kids.size()) - 1, 0, line); return;
        case NK::Index: expr(*n.kids[0]); expr(*n.kids[1]); emit(Opc::GetIndex, 0, 0, line); return;
        case NK::Field: expr(*n.kids[0]); emit(Opc::GetField, name(n.s), 0, line); return;
        case NK::OptField: expr(*n.kids[0]); emit(Opc::OptField, name(n.s), 0, line); return;
        case NK::Lambda: emit(Opc::Closure, node(keep(n))); return;
        case NK::Shell: {
            const Node& b = *n.kids[0]; int k = 0;
            if (b.k == NK::Interp) { for (auto& p : b.kids) expr(*p); k = int(b.kids.size()); }
            emit(Opc::Shell, node(keep(n)), k, line); return;
        }
        case NK::Is: expr(*n.kids[0]); emit(Opc::Is, node(keep(n))); return;
        case NK::Spawn: for (auto& k : n.kids) expr(*k); emit(Opc::Spawn, int(n.kids.size()) - 1, 0, line); return;
        case NK::Await: expr(*n.kids[0]); emit(Opc::Await, 0, 0, line); return;
        case NK::Super: emit(Opc::Super, node(keep(n)), 0, line); return;
        default: emit(Opc::EvalNode, node(keep(n)), 0, line); return;   // Move and other rare forms: tree-walker
        }
    }
    const FnDecl& fd_;
    Chunk c_;
    std::vector<Loop> loops_;
    int line_ = 0;
public:
    std::vector<std::pair<int, int>> exec_targets_;
};

const char* opname(Opc o) {
    static const char* n[] = {"CONST", "NIL", "TRUE", "FALSE", "POP", "DUP", "LOAD_LOCAL", "STORE_LOCAL", "LOAD_GLOBAL", "STORE_GLOBAL",
        "DEFINE_GLOBAL", "DEFINE_CONST", "BINARY", "UNARY", "JUMP", "JUMP_IF_FALSE", "JUMP_IF_FALSE_KEEP", "JUMP_IF_TRUE_KEEP",
        "JUMP_IF_NOT_NIL_KEEP", "CALL", "GET_FIELD", "OPT_FIELD", "SET_FIELD", "GET_INDEX", "SET_INDEX", "MAKE_LIST", "MAKE_MAP",
        "CLOSURE", "INTERP", "ITER", "ITER_NEXT", "ITER_NEXT2", "RETURN", "RETURN_NIL", "CHECK_TYPE", "EXEC_NODE", "IS", "SPAWN",
        "AWAIT", "SHELL", "THROW", "LINE", "SUPER", "COPY", "EVAL_NODE", "CLEAR_LOCAL"};
    return n[int(o)];
}

} // namespace

// exec targets live in the chunk's names table tail? No: keep them in a side vector encoded as constants.
std::shared_ptr<const Chunk> compile(const FnDecl& fd) {
    Compiler c(fd);
    auto chunk = c.run();
    // Append exec targets as int constant pairs; ExecNode.b indexes pairs starting at targets_base.
    auto out = std::make_shared<Chunk>(*chunk);
    int base = int(out->consts.size());
    for (auto& [b, k] : c.exec_targets_) { out->consts.push_back(std::int64_t(b)); out->consts.push_back(std::int64_t(k)); }
    for (auto& ins : out->code) if (ins.op == Opc::ExecNode) ins.b = base + 2 * ins.b;
    verify(*out);
    return out;
}

void verify(const Chunk& c) {
    const int n = int(c.code.size());
    auto bad = [&](int pc, const std::string& m) { throw std::runtime_error(std::format("bytecode verify failed in {} at {}: {}", c.name, pc, m)); };
    if (n == 0 || c.lines.size() != c.code.size()) bad(0, "empty or malformed chunk");
    std::vector<int> depth(std::size_t(n) + 1, -1);
    std::vector<int> work{0}; depth[0] = 0;
    auto flow = [&](int pc, int to, int d) {
        if (to < 0 || to > n) bad(pc, "jump target out of range");
        if (d < 0) bad(pc, "stack underflow");
        if (to == n) return;   // falling off the end is caught below
        if (depth[std::size_t(to)] == -1) { depth[std::size_t(to)] = d; work.push_back(to); }
        else if (depth[std::size_t(to)] != d) bad(pc, std::format("inconsistent stack depth at {} ({} vs {})", to, depth[std::size_t(to)], d));
    };
    while (!work.empty()) {
        int pc = work.back(); work.pop_back();
        int d = depth[std::size_t(pc)];
        const Instr& i = c.code[std::size_t(pc)];
        auto need = [&](int k) { if (d < k) bad(pc, std::string("stack underflow in ") + opname(i.op)); };
        auto idx = [&](int v, std::size_t lim, const char* what) { if (v < 0 || std::size_t(v) >= lim) bad(pc, std::string(what) + " index out of range"); };
        int nx = pc + 1;
        switch (i.op) {
        case Opc::Const: idx(i.a, c.consts.size(), "constant"); flow(pc, nx, d + 1); break;
        case Opc::Nil: case Opc::True: case Opc::False: flow(pc, nx, d + 1); break;
        case Opc::Pop: need(1); flow(pc, nx, d - 1); break;
        case Opc::Dup: need(1); flow(pc, nx, d + 1); break;
        case Opc::LoadLocal: if (i.a < 0 || i.b < 0 || (i.a == 0 && i.b >= c.nslots)) bad(pc, "local slot out of range"); flow(pc, nx, d + 1); break;
        case Opc::StoreLocal: need(1); if (i.a < 0 || i.b < 0 || (i.a == 0 && i.b >= c.nslots)) bad(pc, "local slot out of range"); flow(pc, nx, d - 1); break;
        case Opc::LoadGlobal: idx(i.a, c.names.size(), "name"); flow(pc, nx, d + 1); break;
        case Opc::StoreGlobal: case Opc::DefineGlobal: case Opc::DefineConst: need(1); idx(i.a, c.names.size(), "name"); flow(pc, nx, d - 1); break;
        case Opc::Binary: need(2); if (i.a < 0 || i.a >= int(Op::None)) bad(pc, "bad operator"); flow(pc, nx, d - 1); break;
        case Opc::Unary: need(1); flow(pc, nx, d); break;
        case Opc::Jump: flow(pc, i.a, d); break;
        case Opc::JumpIfFalse: need(1); flow(pc, i.a, d - 1); flow(pc, nx, d - 1); break;
        case Opc::JumpIfFalseKeep: case Opc::JumpIfTrueKeep: case Opc::JumpIfNotNilKeep: need(1); flow(pc, i.a, d); flow(pc, nx, d - 1); break;
        case Opc::Call: case Opc::Spawn: if (i.a < 0) bad(pc, "bad argc"); need(i.a + 1); flow(pc, nx, d - i.a); break;
        case Opc::GetField: case Opc::OptField: need(1); idx(i.a, c.names.size(), "name"); flow(pc, nx, d); break;
        case Opc::SetField: need(2); idx(i.a, c.names.size(), "name"); flow(pc, nx, d - 2); break;
        case Opc::GetIndex: need(2); flow(pc, nx, d - 1); break;
        case Opc::SetIndex: need(3); flow(pc, nx, d - 3); break;
        case Opc::MakeList: case Opc::Interp: if (i.a < 0) bad(pc, "bad count"); need(i.a); flow(pc, nx, d - i.a + 1); break;
        case Opc::MakeMap: if (i.a < 0) bad(pc, "bad count"); need(2 * i.a); flow(pc, nx, d - 2 * i.a + 1); break;
        case Opc::Closure: idx(i.a, c.nodes.size(), "node"); if (!c.nodes[std::size_t(i.a)]->fn) bad(pc, "closure node has no function"); flow(pc, nx, d + 1); break;
        case Opc::Iter: need(1); flow(pc, nx, d); break;
        case Opc::IterNext: need(1); flow(pc, i.a, d - 1); flow(pc, nx, d + 1); break;
        case Opc::IterNext2: need(1); flow(pc, i.a, d - 1); flow(pc, nx, d + 2); break;
        case Opc::Return: case Opc::Throw: need(1); break;
        case Opc::ReturnNil: break;
        case Opc::CheckType: need(1); idx(i.a, c.nodes.size(), "node"); if (!c.nodes[std::size_t(i.a)]->type) bad(pc, "type node missing"); flow(pc, nx, d); break;
        case Opc::Is: need(1); idx(i.a, c.nodes.size(), "node"); if (!c.nodes[std::size_t(i.a)]->type) bad(pc, "type node missing"); flow(pc, nx, d); break;
        case Opc::ExecNode: {
            idx(i.a, c.nodes.size(), "node"); idx(i.b + 1, c.consts.size(), "exec target");
            flow(pc, nx, d);
            for (int k = 0; k < 2; ++k) { auto* t = std::get_if<std::int64_t>(&c.consts[std::size_t(i.b + k)]); if (!t) bad(pc, "exec target type"); if (*t >= 0) flow(pc, int(*t), d); }
            break;
        }
        case Opc::Await: need(1); flow(pc, nx, d); break;
        case Opc::Shell: idx(i.a, c.nodes.size(), "node"); if (i.b < 0) bad(pc, "bad part count"); need(i.b); flow(pc, nx, d - i.b + 1); break;
        case Opc::Line: flow(pc, nx, d); break;
        case Opc::Super: case Opc::EvalNode: idx(i.a, c.nodes.size(), "node"); flow(pc, nx, d + 1); break;
        case Opc::Copy: need(1); flow(pc, nx, d); break;
        case Opc::ClearLocal: if (i.b < 0 || i.b >= c.nslots) bad(pc, "local slot out of range"); flow(pc, nx, d); break;
        default: bad(pc, "unknown opcode");
        }
    }
    if (depth[std::size_t(n - 1)] == -1 && c.code.back().op != Opc::ReturnNil) bad(n - 1, "unreachable end");
    for (int pc = 0; pc < n; ++pc) {
        if (depth[std::size_t(pc)] == -1) continue;
        auto op = c.code[std::size_t(pc)].op;
        bool terminal = op == Opc::Return || op == Opc::ReturnNil || op == Opc::Throw || op == Opc::Jump;
        if (pc == n - 1 && !terminal) bad(pc, "code falls off the end");
    }
}

std::string disassemble(const Chunk& c) {
    std::string s = std::format("== {} ({} slots, {} consts, {} instrs)\n", c.name, c.nslots, c.consts.size(), c.code.size());
    for (std::size_t pc = 0; pc < c.code.size(); ++pc) {
        auto& i = c.code[pc];
        s += std::format("{:4} {:4}  {:<20}", pc, c.lines[pc], opname(i.op));
        switch (i.op) {
        case Opc::Const: {
            auto& v = c.consts[std::size_t(i.a)];
            if (auto* x = std::get_if<std::int64_t>(&v)) s += std::to_string(*x);
            else if (auto* d = std::get_if<double>(&v)) s += std::format("{}", *d);
            else if (auto* t = as<String>(v)) s += "\"" + t->s + "\"";
            break;
        }
        case Opc::LoadGlobal: case Opc::StoreGlobal: case Opc::DefineGlobal: case Opc::DefineConst: case Opc::GetField: case Opc::OptField: case Opc::SetField:
            s += c.names[std::size_t(i.a)]; break;
        case Opc::LoadLocal: case Opc::StoreLocal: s += std::format("depth={} slot={}", i.a, i.b); break;
        case Opc::Closure: s += "<fn " + c.nodes[std::size_t(i.a)]->fn->name + ">"; break;
        default: if (i.a || i.b) s += std::format("{} {}", i.a, i.b);
        }
        s += "\n";
    }
    return s;
}

Value vm_run(Interp& in, const Chunk& c, const Ref<Env>& env) {
    std::vector<Value> st; st.reserve(16);
    auto pop = [&]() { Value v = std::move(st.back()); st.pop_back(); return v; };
    std::size_t pc = 0;
    const std::size_t n = c.code.size();
    while (pc < n) {
        const Instr& i = c.code[pc];
        ++pc;
        switch (i.op) {
        case Opc::Const: { const Value& k = c.consts[std::size_t(i.a)]; if (auto* ks = as<String>(k)) st.push_back(in.new_string(ks->s)); else st.push_back(k); break; }
        case Opc::Nil: st.emplace_back(Nil{}); break;
        case Opc::True: st.emplace_back(true); break;
        case Opc::False: st.emplace_back(false); break;
        case Opc::Pop: st.pop_back(); break;
        case Opc::Dup: st.push_back(st.back()); break;
        case Opc::LoadLocal: {
            Env* e = env.get(); for (int d = i.a; d > 0 && e; --d) e = e->parent.get();
            if (!e) in.raise("NameError", "bad scope");
            const Value& x = e->slots.at(std::size_t(i.b));
            if (is_moved(x)) in.raise("BorrowError", "use of a moved-from value");
            st.push_back(x); break;
        }
        case Opc::StoreLocal: {
            Env* e = env.get(); for (int d = i.a; d > 0 && e; --d) e = e->parent.get();
            if (!e) in.raise("NameError", "bad scope");
            e->slots.at(std::size_t(i.b)) = in.share(pop()); break;
        }
        case Opc::LoadGlobal: st.push_back(in.load(VarRef{c.names[std::size_t(i.a)], -1, -1}, env)); break;
        case Opc::StoreGlobal: in.store(VarRef{c.names[std::size_t(i.a)], -1, -1}, env, pop()); break;
        case Opc::DefineGlobal: in.store(VarRef{c.names[std::size_t(i.a)], -1, -1}, env, pop(), true, false); break;
        case Opc::DefineConst: in.store(VarRef{c.names[std::size_t(i.a)], -1, -1}, env, pop(), true, true); break;
        case Opc::Binary: { Value b = pop(); Value a = pop(); in.line = c.lines[pc - 1]; st.push_back(in.binary(Op(i.a), a, b)); break; }
        case Opc::Unary: { Value a = pop(); st.push_back(i.a == 2 ? Value(Interp::truthy(a)) : in.unary(i.a == 1 ? "not" : i.a == 3 ? "~" : "-", a)); break; }
        case Opc::Jump: pc = std::size_t(i.a); break;
        case Opc::JumpIfFalse: { Value v = pop(); if (!Interp::truthy(v)) pc = std::size_t(i.a); break; }
        case Opc::JumpIfFalseKeep: if (!Interp::truthy(st.back())) pc = std::size_t(i.a); else st.pop_back(); break;
        case Opc::JumpIfTrueKeep: if (Interp::truthy(st.back())) pc = std::size_t(i.a); else st.pop_back(); break;
        case Opc::JumpIfNotNilKeep: if (!is_nil(st.back())) pc = std::size_t(i.a); else st.pop_back(); break;
        case Opc::Call: {
            std::size_t argc = std::size_t(i.a);
            std::vector<Value> args; args.reserve(argc);
            for (std::size_t k = st.size() - argc; k < st.size(); ++k) args.push_back(std::move(st[k]));
            st.resize(st.size() - argc);
            Value f = pop();
            st.push_back(in.call(f, args, c.lines[pc - 1]));
            break;
        }
        case Opc::GetField: { Value o = pop(); in.line = c.lines[pc - 1]; st.push_back(in.get_field(o, c.names[std::size_t(i.a)])); break; }
        case Opc::OptField: { Value o = pop(); st.push_back(in.get_field(o, c.names[std::size_t(i.a)], true)); break; }
        case Opc::SetField: { Value v = pop(); Value o = pop(); in.set_field(o, c.names[std::size_t(i.a)], std::move(v)); break; }
        case Opc::GetIndex: { Value k = pop(); Value o = pop(); in.line = c.lines[pc - 1]; st.push_back(in.get_index(o, k)); break; }
        case Opc::SetIndex: { Value v = pop(); Value k = pop(); Value o = pop(); in.set_index(o, k, std::move(v)); break; }
        case Opc::MakeList: {
            auto l = make<List>(); std::size_t k = std::size_t(i.a);
            for (std::size_t j = st.size() - k; j < st.size(); ++j) l->push(in.share(st[j]));
            st.resize(st.size() - k); st.push_back(obj(l)); break;
        }
        case Opc::MakeMap: {
            auto m = make<Map>(); std::size_t k = 2 * std::size_t(i.a);
            for (std::size_t j = st.size() - k; j < st.size(); j += 2) {
                if (!key_of(st[j])) in.raise("TypeError", "map keys must be nil, bool, numbers or strings");
                m->set(st[j], in.share(st[j + 1]));
            }
            st.resize(st.size() - k); st.push_back(obj(m)); break;
        }
        case Opc::Closure: st.push_back(in.make_closure(c.nodes[std::size_t(i.a)]->fn, env)); break;
        case Opc::Interp: {
            std::size_t k = std::size_t(i.a);
            std::vector<Value> parts(st.end() - std::ptrdiff_t(k), st.end());
            st.resize(st.size() - k); st.push_back(in.interpolate(parts)); break;
        }
        case Opc::Iter: { Value v = pop(); st.push_back(Value(in.iterate(v))); break; }
        case Opc::IterNext: case Opc::IterNext2: {
            auto* r = std::get_if<Ref<Obj>>(&st.back());
            if (!r || !*r) in.raise("TypeError", "not an iterator");
            Value a, b;
            bool two = i.op == Opc::IterNext2;
            if (!in.iter_next(**r, a, two ? &b : nullptr)) { st.pop_back(); pc = std::size_t(i.a); break; }
            in.step();
            if (two) { st.push_back(std::move(a)); st.push_back(std::move(b)); } else st.push_back(std::move(a));
            break;
        }
        case Opc::Return: return pop();
        case Opc::ReturnNil: return Nil{};
        case Opc::CheckType: { auto& nd = *c.nodes[std::size_t(i.a)]; in.check_type(st.back(), *nd.type, "let " + nd.s); break; }
        case Opc::Is: { Value v = pop(); st.push_back(in.conforms(v, *c.nodes[std::size_t(i.a)]->type)); break; }
        case Opc::ExecNode: {
            Flow f = in.exec(*c.nodes[std::size_t(i.a)], env);
            if (f == Flow::Return) { Value r = std::move(in.ret); in.ret = Nil{}; return r; }
            if (f == Flow::Break || f == Flow::Continue) {
                auto t = std::get<std::int64_t>(c.consts[std::size_t(i.b + (f == Flow::Break ? 0 : 1))]);
                if (t < 0) in.raise("SyntaxError", "break/continue outside a loop");
                pc = std::size_t(t);
            }
            break;
        }
        case Opc::Spawn: {
            std::size_t argc = std::size_t(i.a);
            std::vector<Value> args(st.end() - std::ptrdiff_t(argc), st.end());
            st.resize(st.size() - argc);
            Value f = pop();
            st.push_back(in.spawn(f, std::move(args)));
            break;
        }
        case Opc::Await: { Value t = pop(); st.push_back(in.await(t)); break; }
        case Opc::Shell: {
            std::size_t k = std::size_t(i.b);
            std::vector<Value> parts(st.end() - std::ptrdiff_t(k), st.end());
            st.resize(st.size() - k);
            in.line = c.lines[pc - 1];
            st.push_back(in.shell(*c.nodes[std::size_t(i.a)], parts));
            break;
        }
        case Opc::Throw: { Value v = pop(); throw ScriptError(v, in.error_message(v)); }
        case Opc::Line: in.line = i.a; in.step(); break;
        case Opc::Super: case Opc::EvalNode: in.line = c.lines[pc - 1]; st.push_back(in.eval(*c.nodes[std::size_t(i.a)], env)); break;
        case Opc::Copy: { Value v = pop(); st.push_back(in.copy_value(v, -1)); break; }
        case Opc::ClearLocal: { Value dead = std::move(env->slots.at(std::size_t(i.b))); env->slots[std::size_t(i.b)] = Nil{}; dead = Nil{}; if (finalizers_pending()) in.run_finalizers(); break; }
        }
    }
    return Nil{};
}

} // namespace forge::script
