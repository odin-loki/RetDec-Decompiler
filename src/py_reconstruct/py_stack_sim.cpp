/**
 * @file src/py_reconstruct/py_stack_sim.cpp
 * @brief CPython stack simulation → Python expression/statement AST.
 */

#include <memory>
#include "retdec/py_reconstruct/py_stack_sim.h"
#include "retdec/pyc_parser/py_opcodes.h"

#include <algorithm>
#include <cassert>
#include <sstream>

namespace retdec {
namespace py_reconstruct {

using namespace pyc_parser;

// ─── PyStackSimulator ────────────────────────────────────────────────────────

PyStackSimulator::PyStackSimulator(const PyCodeObject& code, Options opts)
    : code_(code), opts_(opts) {}

// ─── decode ──────────────────────────────────────────────────────────────────

std::vector<PyStackSimulator::RawInstr> PyStackSimulator::decode() const {
    std::vector<RawInstr> result;
    const auto& bc  = code_.co_code;
    const bool is311 = code_.version.atLeast(3, 11);
    size_t pos = 0;
    int32_t extArg = 0;

    while (pos + 1 < bc.size()) {
        uint8_t op  = bc[pos];
        int32_t arg = static_cast<int32_t>(bc[pos+1]);
        pos += 2;

        const uint8_t extOp = is311 ? 144 : 90; // EXTENDED_ARG
        if (op == extOp) {
            extArg = (extArg | arg) << 8;
            continue;
        }
        int32_t fullArg = extArg | arg;
        extArg = 0;

        OpcodeInfo info = opcodeInfo(op, code_.version);
        result.push_back({std::string(info.name), fullArg,
                          static_cast<uint32_t>(pos - 2)});
    }
    return result;
}

// ─── findLeaders ─────────────────────────────────────────────────────────────

std::vector<uint32_t> PyStackSimulator::findLeaders(
        const std::vector<RawInstr>& instrs) const {
    std::vector<uint32_t> leaders = {0};
    const bool is311 = code_.version.atLeast(3, 11);

    for (size_t i = 0; i < instrs.size(); ++i) {
        const auto& instr = instrs[i];
        OpcodeInfo info = opcodeInfo(0, code_.version);
        // Re-query by name
        const std::string& nm = instr.name;
        uint32_t nextOff = (i + 1 < instrs.size()) ? instrs[i+1].offset : UINT32_MAX;

        bool isJump = nm.find("JUMP") != std::string::npos ||
                      nm.find("POP_JUMP") != std::string::npos ||
                      nm == "FOR_ITER" || nm == "SEND";

        if (isJump) {
            // Resolve target
            int32_t target = instr.arg;
            if (nm.find("FORWARD") != std::string::npos)
                target = static_cast<int32_t>(instr.offset) + 2 + instr.arg * 2;
            else if (nm.find("BACKWARD") != std::string::npos)
                target = static_cast<int32_t>(instr.offset) + 2 - instr.arg * 2;
            else if (is311)
                target = static_cast<int32_t>(instr.offset) + 2 + instr.arg * 2;

            leaders.push_back(static_cast<uint32_t>(std::max(0, target)));
            if (nextOff != UINT32_MAX) leaders.push_back(nextOff);
        }

        if (nm == "RETURN_VALUE" || nm == "RETURN_CONST" ||
            nm == "RAISE_VARARGS" || nm == "RERAISE") {
            if (nextOff != UINT32_MAX) leaders.push_back(nextOff);
        }
    }
    std::sort(leaders.begin(), leaders.end());
    leaders.erase(std::unique(leaders.begin(), leaders.end()), leaders.end());
    return leaders;
}

// ─── constFromIdx ────────────────────────────────────────────────────────────

PyExprPtr PyStackSimulator::constFromIdx(int32_t idx) const {
    if (idx < 0 || static_cast<size_t>(idx) >= code_.co_consts.size())
        return makeNone();

    const auto& c = code_.co_consts[static_cast<size_t>(idx)];
    switch (c.kind) {
    case PyCodeObject::Const::Kind::None:     return makeNone();
    case PyCodeObject::Const::Kind::True: {
        auto e = std::make_shared<PyExpr>();
        e->kind = PyExpr::Kind::Constant;
        e->constKind = PyExpr::ConstKind::True_;
        return e;
    }
    case PyCodeObject::Const::Kind::False: {
        auto e = std::make_shared<PyExpr>();
        e->kind = PyExpr::Kind::Constant;
        e->constKind = PyExpr::ConstKind::False_;
        return e;
    }
    case PyCodeObject::Const::Kind::Ellipsis: {
        auto e = std::make_shared<PyExpr>();
        e->kind = PyExpr::Kind::Constant;
        e->constKind = PyExpr::ConstKind::Ellipsis_;
        return e;
    }
    case PyCodeObject::Const::Kind::Int:     return makeConst(c.ival);
    case PyCodeObject::Const::Kind::Float:   return makeConst(c.fval);
    case PyCodeObject::Const::Kind::Str:
    case PyCodeObject::Const::Kind::Unicode: return makeConst(c.sval, false);
    case PyCodeObject::Const::Kind::Bytes:   return makeConst(c.sval, true);
    case PyCodeObject::Const::Kind::Code:
        // Represent nested code objects by their co_name so MAKE_FUNCTION can name the function.
        return makeConst(c.code ? c.code->co_name : "<code>", false);
    default:                                 return makeNone();
    }
}

std::string PyStackSimulator::nameFromIdx(
        int32_t idx, const std::vector<std::string>& table) const {
    if (idx >= 0 && static_cast<size_t>(idx) < table.size())
        return table[static_cast<size_t>(idx)];
    return "?";
}

// ─── popExpr / pushExpr ──────────────────────────────────────────────────────

PyExprPtr PyStackSimulator::popExpr(Stack& stack) const {
    if (stack.empty()) {
        warn("Stack underflow");
        return makeName("_STACK_UNDERFLOW_");
    }
    auto e = stack.back().expr;
    stack.pop_back();
    return e;
}

void PyStackSimulator::pushExpr(Stack& stack, PyExprPtr e) const {
    stack.push_back({std::move(e)});
}

// ─── buildBinOp ──────────────────────────────────────────────────────────────

PyExprPtr PyStackSimulator::buildBinOp(const std::string& opName,
                                        PyExprPtr lhs, PyExprPtr rhs) const {
    static const std::unordered_map<std::string, BinOp> kMap = {
        {"BINARY_ADD",           BinOp::Add},
        {"BINARY_SUBTRACT",      BinOp::Sub},
        {"BINARY_MULTIPLY",      BinOp::Mult},
        {"BINARY_MATRIX_MULTIPLY",BinOp::MatMult},
        {"BINARY_TRUE_DIVIDE",   BinOp::Div},
        {"BINARY_FLOOR_DIVIDE",  BinOp::FloorDiv},
        {"BINARY_MODULO",        BinOp::Mod},
        {"BINARY_POWER",         BinOp::Pow},
        {"BINARY_LSHIFT",        BinOp::LShift},
        {"BINARY_RSHIFT",        BinOp::RShift},
        {"BINARY_AND",           BinOp::BitAnd},
        {"BINARY_OR",            BinOp::BitOr},
        {"BINARY_XOR",           BinOp::BitXor},
        // inplace
        {"INPLACE_ADD",          BinOp::Add},
        {"INPLACE_SUBTRACT",     BinOp::Sub},
        {"INPLACE_MULTIPLY",     BinOp::Mult},
        {"INPLACE_MATRIX_MULTIPLY",BinOp::MatMult},
        {"INPLACE_TRUE_DIVIDE",  BinOp::Div},
        {"INPLACE_FLOOR_DIVIDE", BinOp::FloorDiv},
        {"INPLACE_MODULO",       BinOp::Mod},
        {"INPLACE_POWER",        BinOp::Pow},
        {"INPLACE_LSHIFT",       BinOp::LShift},
        {"INPLACE_RSHIFT",       BinOp::RShift},
        {"INPLACE_AND",          BinOp::BitAnd},
        {"INPLACE_OR",           BinOp::BitOr},
        {"INPLACE_XOR",          BinOp::BitXor},
    };
    auto e = std::make_shared<PyExpr>();
    e->kind = PyExpr::Kind::BinOp;
    auto it = kMap.find(opName);
    e->binOp = (it != kMap.end()) ? it->second : BinOp::Add;
    e->children = {std::move(lhs), std::move(rhs)};
    return e;
}

// ─── buildCollection ─────────────────────────────────────────────────────────

PyExprPtr PyStackSimulator::buildCollection(const std::string& opName,
                                             int32_t count, Stack& stack) const {
    ExprList elts;
    for (int32_t i = 0; i < count; ++i)
        elts.insert(elts.begin(), popExpr(stack));

    auto e = std::make_shared<PyExpr>();
    e->values = std::move(elts);

    if (opName == "BUILD_LIST")       e->kind = PyExpr::Kind::List;
    else if (opName == "BUILD_TUPLE") e->kind = PyExpr::Kind::Tuple;
    else if (opName == "BUILD_SET")   e->kind = PyExpr::Kind::Set;
    else                              e->kind = PyExpr::Kind::Tuple;
    return e;
}

// ─── buildCompare ────────────────────────────────────────────────────────────

PyExprPtr PyStackSimulator::buildCompare(int32_t cmpIdx,
                                          PyExprPtr lhs, PyExprPtr rhs) const {
    static const CmpOp kCmpOps[] = {
        CmpOp::Lt, CmpOp::LtE, CmpOp::Eq, CmpOp::NotEq,
        CmpOp::Gt, CmpOp::GtE, CmpOp::In, CmpOp::NotIn,
        CmpOp::Is, CmpOp::IsNot
    };
    auto e = std::make_shared<PyExpr>();
    e->kind = PyExpr::Kind::Compare;
    e->children = {std::move(lhs)};
    e->values   = {std::move(rhs)};
    CmpOp op = (cmpIdx >= 0 && cmpIdx < 10) ? kCmpOps[cmpIdx] : CmpOp::Eq;
    e->cmpOps = {op};
    return e;
}

// ─── applyInstr ──────────────────────────────────────────────────────────────

bool PyStackSimulator::applyInstr(const RawInstr& instr,
                                    Stack& stack, StmtList& stmts) {
    const std::string& nm = instr.name;
    int32_t arg = instr.arg;

    // ── LOAD operations ─────────────────────────────────────────────────────
    if (nm == "LOAD_CONST") {
        pushExpr(stack, constFromIdx(arg));
        return true;
    }
    if (nm == "LOAD_FAST" || nm == "LOAD_FAST_CHECK") {
        pushExpr(stack, makeName(nameFromIdx(arg, code_.co_varnames)));
        return true;
    }
    if (nm == "LOAD_NAME") {
        // LOAD_NAME arg is always a direct index into co_names (no bit-shifting in any version).
        pushExpr(stack, makeName(nameFromIdx(arg, code_.co_names)));
        return true;
    }
    if (nm == "LOAD_GLOBAL") {
        // In 3.11+: arg encodes name index in bits [1:], bit 0 = push NULL sentinel.
        int nameIdx = code_.version.atLeast(3, 11) ? (arg >> 1) : arg;
        if (code_.version.atLeast(3, 11) && (arg & 1)) {
            pushExpr(stack, makeName("_null_")); // NULL placeholder consumed by CALL
        }
        pushExpr(stack, makeName(nameFromIdx(nameIdx, code_.co_names)));
        return true;
    }
    if (nm == "LOAD_ATTR") {
        auto obj = popExpr(stack);
        int nameIdx = code_.version.atLeast(3, 11) ? (arg >> 1) : arg;
        // In 3.11+, LOAD_ATTR with arg&1 is a method load: pushes [self, method]
        if (code_.version.atLeast(3, 11) && (arg & 1)) {
            pushExpr(stack, obj); // self (pushed first, consumed by CALL)
        }
        pushExpr(stack, makeAttr(obj, nameFromIdx(nameIdx, code_.co_names)));
        return true;
    }
    if (nm == "LOAD_METHOD") {
        auto obj = popExpr(stack);
        pushExpr(stack, makeAttr(obj, nameFromIdx(arg, code_.co_names)));
        pushExpr(stack, makeName("_self_")); // placeholder for method dispatch
        return true;
    }
    if (nm == "LOAD_DEREF" || nm == "LOAD_CLOSURE" || nm == "LOAD_CLASSDEREF") {
        size_t ci = static_cast<size_t>(arg);
        std::string n;
        if (ci < code_.co_cellvars.size())
            n = code_.co_cellvars[ci];
        else {
            ci -= code_.co_cellvars.size();
            if (ci < code_.co_freevars.size()) n = code_.co_freevars[ci];
            else n = "?";
        }
        pushExpr(stack, makeName(n));
        return true;
    }
    if (nm == "LOAD_BUILD_CLASS") {
        pushExpr(stack, makeName("__build_class__"));
        return true;
    }
    if (nm == "LOAD_ASSERTION_ERROR") {
        pushExpr(stack, makeName("AssertionError"));
        return true;
    }

    // ── STORE operations ────────────────────────────────────────────────────
    if (nm == "STORE_FAST") {
        auto val = popExpr(stack);
        stmts.push_back(makeAssign(
            {makeName(nameFromIdx(arg, code_.co_varnames), ExprCtx::Store)}, val));
        return true;
    }
    if (nm == "STORE_NAME" || nm == "STORE_GLOBAL") {
        auto val = popExpr(stack);
        stmts.push_back(makeAssign(
            {makeName(nameFromIdx(arg, code_.co_names), ExprCtx::Store)}, val));
        return true;
    }
    if (nm == "STORE_ATTR") {
        auto val = popExpr(stack);
        auto obj = popExpr(stack);
        auto tgt = makeAttr(obj, nameFromIdx(arg, code_.co_names), ExprCtx::Store);
        stmts.push_back(makeAssign({tgt}, val));
        return true;
    }
    if (nm == "STORE_SUBSCR") {
        // CPython STORE_SUBSCR: TOS1[TOS] = TOS2.
        // Stack order: TOS=key, TOS1=container, TOS2=value.
        auto key = popExpr(stack);  // TOS  = index/key
        auto obj = popExpr(stack);  // TOS1 = container
        auto val = popExpr(stack);  // TOS2 = value
        auto sub = std::make_shared<PyExpr>();
        sub->kind = PyExpr::Kind::Subscript;
        sub->ctx  = ExprCtx::Store;
        sub->children = {obj, key};
        stmts.push_back(makeAssign({sub}, val));
        return true;
    }
    if (nm == "STORE_DEREF") {
        auto val = popExpr(stack);
        size_t ci = static_cast<size_t>(arg);
        std::string n;
        if (ci < code_.co_cellvars.size()) n = code_.co_cellvars[ci];
        else {
            ci -= code_.co_cellvars.size();
            n = (ci < code_.co_freevars.size()) ? code_.co_freevars[ci] : "?";
        }
        stmts.push_back(makeAssign({makeName(n, ExprCtx::Store)}, val));
        return true;
    }

    // ── DELETE operations ───────────────────────────────────────────────────
    if (nm == "DELETE_FAST" || nm == "DELETE_NAME" || nm == "DELETE_GLOBAL") {
        auto s = std::make_shared<PyStmt>();
        s->kind = PyStmt::Kind::Delete;
        const auto& table = (nm == "DELETE_FAST") ? code_.co_varnames : code_.co_names;
        s->expr = makeName(nameFromIdx(arg, table));
        stmts.push_back(s);
        return true;
    }

    // ── RETURN ──────────────────────────────────────────────────────────────
    if (nm == "RETURN_VALUE") {
        auto val = stack.empty() ? makeNone() : popExpr(stack);
        stmts.push_back(makeReturn(val));
        return true;
    }
    if (nm == "RETURN_CONST") {
        stmts.push_back(makeReturn(constFromIdx(arg)));
        return true;
    }

    // ── POP_TOP ─────────────────────────────────────────────────────────────
    if (nm == "POP_TOP") {
        if (!stack.empty()) {
            auto e = popExpr(stack);
            // Emit as expression statement if it might have side effects
            // (calls, yield, etc.) but not if it's just a loaded constant/name
            if (e->kind == PyExpr::Kind::Call ||
                e->kind == PyExpr::Kind::Yield ||
                e->kind == PyExpr::Kind::YieldFrom ||
                e->kind == PyExpr::Kind::Await) {
                stmts.push_back(makeExprStmt(e));
            }
        }
        return true;
    }

    // ── BINARY / INPLACE ops ─────────────────────────────────────────────────
    if (nm.rfind("BINARY_", 0) == 0 || nm.rfind("INPLACE_", 0) == 0) {
        if (nm == "BINARY_SUBSCR") {
            auto idx = popExpr(stack);
            auto obj = popExpr(stack);
            auto e = std::make_shared<PyExpr>();
            e->kind = PyExpr::Kind::Subscript;
            e->children = {obj, idx};
            pushExpr(stack, e);
            return true;
        }
        if (nm == "BINARY_OP") {
            // 3.11+: arg encodes the operator
            static const char* kBinaryOpNames[] = {
                "+", "&", "//", "<<", "@", "*", "%", "|", "**",
                ">>", "-", "/", "^", nullptr
            };
            auto rhs = popExpr(stack);
            auto lhs = popExpr(stack);
            // Map arg to BinOp kind
            static const BinOp kBinOps[] = {
                BinOp::Add, BinOp::BitAnd, BinOp::FloorDiv, BinOp::LShift,
                BinOp::MatMult, BinOp::Mult, BinOp::Mod, BinOp::BitOr,
                BinOp::Pow, BinOp::RShift, BinOp::Sub, BinOp::Div,
                BinOp::BitXor
            };
            auto e = std::make_shared<PyExpr>();
            e->kind = PyExpr::Kind::BinOp;
            e->binOp = (arg >= 0 && arg < 13) ? kBinOps[arg] : BinOp::Add;
            e->children = {lhs, rhs};
            pushExpr(stack, e);
            return true;
        }
        auto rhs = popExpr(stack);
        auto lhs = popExpr(stack);
        pushExpr(stack, buildBinOp(nm, lhs, rhs));
        return true;
    }

    // ── UNARY ops ────────────────────────────────────────────────────────────
    if (nm.rfind("UNARY_", 0) == 0) {
        auto operand = popExpr(stack);
        auto e = std::make_shared<PyExpr>();
        e->kind = PyExpr::Kind::UnaryOp;
        if (nm == "UNARY_NOT")    e->unaryOp = UnaryOp::Not;
        else if (nm == "UNARY_INVERT") e->unaryOp = UnaryOp::Invert;
        else if (nm == "UNARY_NEGATIVE") e->unaryOp = UnaryOp::USub;
        else e->unaryOp = UnaryOp::UAdd;
        e->children = {operand};
        pushExpr(stack, e);
        return true;
    }

    // ── COMPARE_OP ──────────────────────────────────────────────────────────
    if (nm == "COMPARE_OP") {
        auto rhs = popExpr(stack);
        auto lhs = popExpr(stack);
        pushExpr(stack, buildCompare(arg, lhs, rhs));
        return true;
    }
    if (nm == "IS_OP") {
        auto rhs = popExpr(stack);
        auto lhs = popExpr(stack);
        auto e = std::make_shared<PyExpr>();
        e->kind = PyExpr::Kind::Compare;
        e->cmpOps = {arg == 0 ? CmpOp::Is : CmpOp::IsNot};
        e->children = {lhs};
        e->values   = {rhs};
        pushExpr(stack, e);
        return true;
    }
    if (nm == "CONTAINS_OP") {
        auto rhs = popExpr(stack);
        auto lhs = popExpr(stack);
        auto e = std::make_shared<PyExpr>();
        e->kind = PyExpr::Kind::Compare;
        e->cmpOps = {arg == 0 ? CmpOp::In : CmpOp::NotIn};
        e->children = {lhs};
        e->values   = {rhs};
        pushExpr(stack, e);
        return true;
    }

    // ── BUILD_* ──────────────────────────────────────────────────────────────
    if (nm == "BUILD_LIST" || nm == "BUILD_TUPLE" || nm == "BUILD_SET") {
        pushExpr(stack, buildCollection(nm, arg, stack));
        return true;
    }
    if (nm == "BUILD_MAP") {
        auto e = std::make_shared<PyExpr>();
        e->kind = PyExpr::Kind::Dict;
        for (int i = 0; i < arg; ++i) {
            auto v = popExpr(stack);
            auto k = popExpr(stack);
            e->values.insert(e->values.begin(), v);
            e->keys.insert(e->keys.begin(), k);
        }
        pushExpr(stack, e);
        return true;
    }
    if (nm == "BUILD_CONST_KEY_MAP") {
        auto keys_tuple = popExpr(stack);
        ExprList vals;
        for (int i = 0; i < arg; ++i)
            vals.insert(vals.begin(), popExpr(stack));
        auto e = std::make_shared<PyExpr>();
        e->kind = PyExpr::Kind::Dict;
        // keys_tuple is a Tuple constant; expand if possible
        if (keys_tuple->kind == PyExpr::Kind::Tuple ||
            keys_tuple->kind == PyExpr::Kind::Constant) {
            e->keys = {keys_tuple};
        }
        e->values = std::move(vals);
        pushExpr(stack, e);
        return true;
    }
    if (nm == "BUILD_STRING") {
        // Concatenate arg string constants → JoinedStr (f-string)
        auto e = std::make_shared<PyExpr>();
        e->kind = PyExpr::Kind::JoinedStr;
        ExprList parts;
        for (int i = 0; i < arg; ++i)
            parts.insert(parts.begin(), popExpr(stack));
        e->values = std::move(parts);
        pushExpr(stack, e);
        return true;
    }
    if (nm == "BUILD_SLICE") {
        PyExprPtr step;
        if (arg == 3) step = popExpr(stack);
        auto upper = popExpr(stack);
        auto lower = popExpr(stack);
        auto e = std::make_shared<PyExpr>();
        e->kind = PyExpr::Kind::Subscript;
        e->isSlice   = true;
        e->sliceLower = lower;
        e->sliceUpper = upper;
        e->sliceStep  = step;
        pushExpr(stack, e);
        return true;
    }

    // ── CALL operations ──────────────────────────────────────────────────────
    if (nm == "CALL_FUNCTION") {
        ExprList posArgs;
        for (int i = 0; i < arg; ++i)
            posArgs.insert(posArgs.begin(), popExpr(stack));
        auto func = popExpr(stack);
        pushExpr(stack, makeCall(func, posArgs));
        return true;
    }
    if (nm == "CALL_FUNCTION_KW") {
        // TOS is tuple of keyword names; TOS1..TOS(N) are values; then positional args; then func
        auto kw_names_expr = popExpr(stack);
        ExprList allArgs;
        for (int i = 0; i < arg; ++i)
            allArgs.insert(allArgs.begin(), popExpr(stack));
        auto func = popExpr(stack);
        auto e = makeCall(func, {});
        // Convert kw args
        int numKw = 0;
        if (kw_names_expr->kind == PyExpr::Kind::Tuple ||
            kw_names_expr->kind == PyExpr::Kind::Constant) {
            numKw = static_cast<int>(kw_names_expr->values.size());
        }
        int numPos = arg - numKw;
        for (int i = 0; i < numPos; ++i)
            e->values.push_back(allArgs[i]);
        for (int i = numPos; i < arg; ++i) {
            PyKeyword kw;
            if (i - numPos < numKw && kw_names_expr->values[i - numPos]) {
                auto& kname = kw_names_expr->values[i - numPos];
                if (kname->kind == PyExpr::Kind::Constant)
                    kw.arg = kname->sval;
            }
            kw.value = allArgs[i];
            e->keywords.push_back(std::move(kw));
        }
        pushExpr(stack, e);
        return true;
    }
    if (nm == "CALL_FUNCTION_EX") {
        PyExprPtr kwargs;
        if (arg & 1) kwargs = popExpr(stack);
        auto args_iter = popExpr(stack);
        auto func = popExpr(stack);
        auto e = makeCall(func, {});
        // *args
        auto star = std::make_shared<PyExpr>();
        star->kind = PyExpr::Kind::Starred;
        star->children = {args_iter};
        e->values.push_back(star);
        if (kwargs) {
            PyKeyword kw;
            // null key = **kwargs
            kw.value = kwargs;
            e->keywords.push_back(std::move(kw));
        }
        pushExpr(stack, e);
        return true;
    }
    if (nm == "CALL_METHOD") {
        ExprList posArgs;
        for (int i = 0; i < arg; ++i)
            posArgs.insert(posArgs.begin(), popExpr(stack));
        auto self_placeholder = popExpr(stack); // _self_ placeholder
        auto method = popExpr(stack);
        (void)self_placeholder;
        pushExpr(stack, makeCall(method, posArgs));
        return true;
    }
    if (nm == "CALL") {
        // 3.11+: arg = nargs (positional + kw)
        ExprList posArgs;
        for (int i = 0; i < arg; ++i)
            posArgs.insert(posArgs.begin(), popExpr(stack));
        auto callable = popExpr(stack);
        // Check for null self (LOAD_METHOD pushed null)
        if (!stack.empty()) {
            auto self = popExpr(stack); // may be NULL_CALLABLE placeholder
            (void)self;
        }
        pushExpr(stack, makeCall(callable, posArgs));
        return true;
    }

    // ── IMPORT ──────────────────────────────────────────────────────────────
    if (nm == "IMPORT_NAME") {
        auto fromlist = popExpr(stack); // TOS
        auto level    = popExpr(stack); // TOS1
        (void)fromlist; (void)level;
        std::string modName = nameFromIdx(arg, code_.co_names);
        auto s = std::make_shared<PyStmt>();
        s->kind = PyStmt::Kind::Import;
        PyAlias alias;
        alias.name = modName;
        s->aliases.push_back(alias);
        stmts.push_back(s);
        pushExpr(stack, makeName(modName));
        return true;
    }
    if (nm == "IMPORT_FROM") {
        auto mod = stack.empty() ? makeName("?") : stack.back().expr;
        std::string fromName = nameFromIdx(arg, code_.co_names);
        auto s = std::make_shared<PyStmt>();
        s->kind   = PyStmt::Kind::ImportFrom;
        s->module = mod->name.empty() ? std::optional<std::string>() : mod->name;
        PyAlias alias;
        alias.name = fromName;
        s->aliases.push_back(alias);
        stmts.push_back(s);
        pushExpr(stack, makeAttr(mod, fromName));
        return true;
    }
    if (nm == "IMPORT_STAR") {
        auto mod = popExpr(stack);
        auto s = std::make_shared<PyStmt>();
        s->kind   = PyStmt::Kind::ImportFrom;
        s->module = mod->name.empty() ? std::optional<std::string>() : mod->name;
        PyAlias alias;
        alias.name = "*";
        s->aliases.push_back(alias);
        stmts.push_back(s);
        return true;
    }

    // ── YIELD ────────────────────────────────────────────────────────────────
    if (nm == "YIELD_VALUE") {
        auto val = stack.empty() ? makeNone() : popExpr(stack);
        auto e = std::make_shared<PyExpr>();
        e->kind = PyExpr::Kind::Yield;
        e->children = {val};
        pushExpr(stack, e);
        return true;
    }
    if (nm == "YIELD_FROM" || nm == "GET_YIELD_FROM_ITER") {
        auto iter = popExpr(stack);
        auto e = std::make_shared<PyExpr>();
        e->kind = PyExpr::Kind::YieldFrom;
        e->children = {iter};
        pushExpr(stack, e);
        return true;
    }

    // ── RAISE ────────────────────────────────────────────────────────────────
    if (nm == "RAISE_VARARGS") {
        auto s = std::make_shared<PyStmt>();
        s->kind = PyStmt::Kind::Raise;
        if (arg >= 2) s->expr2 = popExpr(stack); // cause
        if (arg >= 1) s->expr  = popExpr(stack); // exc
        stmts.push_back(s);
        return true;
    }
    if (nm == "RERAISE") {
        auto s = std::make_shared<PyStmt>();
        s->kind = PyStmt::Kind::Raise;
        stmts.push_back(s);
        return true;
    }

    // ── ASSERT ───────────────────────────────────────────────────────────────
    // (Python compiles assert into a conditional raise; we'd need to detect it
    // at CFG level. For now just leave it as a LOAD_ASSERTION_ERROR + RAISE.)

    // ── STACK MANIPULATION ───────────────────────────────────────────────────
    if (nm == "DUP_TOP") {
        if (!stack.empty()) stack.push_back(stack.back());
        return true;
    }
    if (nm == "DUP_TOP_TWO") {
        if (stack.size() >= 2) {
            auto b = stack[stack.size()-2];
            auto a = stack[stack.size()-1];
            stack.push_back(b);
            stack.push_back(a);
        }
        return true;
    }
    if (nm == "ROT_TWO") {
        if (stack.size() >= 2) std::swap(stack[stack.size()-1], stack[stack.size()-2]);
        return true;
    }
    if (nm == "ROT_THREE") {
        if (stack.size() >= 3) {
            auto tos = stack.back(); stack.pop_back();
            stack.insert(stack.end()-2, tos);
        }
        return true;
    }
    if (nm == "ROT_FOUR") {
        if (stack.size() >= 4) {
            auto tos = stack.back(); stack.pop_back();
            stack.insert(stack.end()-3, tos);
        }
        return true;
    }
    if (nm == "ROT_N") {
        if (stack.size() >= static_cast<size_t>(arg) && arg >= 2) {
            auto tos = stack.back(); stack.pop_back();
            stack.insert(stack.end() - (arg-1), tos);
        }
        return true;
    }
    if (nm == "COPY") {
        if (!stack.empty() && static_cast<size_t>(arg) <= stack.size()) {
            stack.push_back(stack[stack.size() - arg]);
        }
        return true;
    }

    // ── FORMAT_VALUE (f-string part) ─────────────────────────────────────────
    if (nm == "FORMAT_VALUE") {
        PyExprPtr fmt_spec;
        if (arg & 4) fmt_spec = popExpr(stack);
        auto val = popExpr(stack);
        auto e = std::make_shared<PyExpr>();
        e->kind = PyExpr::Kind::FormattedValue;
        e->children = {val};
        e->conversion = (arg & 3) == 1 ? 's' : (arg & 3) == 2 ? 'r' : (arg & 3) == 3 ? 'a' : -1;
        e->format_spec = fmt_spec;
        pushExpr(stack, e);
        return true;
    }

    // ── MAKE_FUNCTION ────────────────────────────────────────────────────────
    if (nm == "MAKE_FUNCTION") {
        // In Python 3.11+: TOS = code object only (qualname embedded in code).
        // In Python 3.10-: TOS = qualname string, TOS1 = code object.
        // Flags: bit0=defaults, bit1=kwdefaults, bit2=annotations, bit3=closure
        if (arg & 8) { if (!stack.empty()) popExpr(stack); } // closure
        if (arg & 4) { if (!stack.empty()) popExpr(stack); } // annotations
        if (arg & 2) { if (!stack.empty()) popExpr(stack); } // kwdefaults
        if (arg & 1) { if (!stack.empty()) popExpr(stack); } // defaults

        PyExprPtr name_expr;
        if (!code_.version.atLeast(3, 11)) {
            // 3.10-: pop qualname then code
            name_expr = stack.empty() ? nullptr : popExpr(stack); // qualname string
            if (!stack.empty()) popExpr(stack); // code object
        } else {
            // 3.11+: pop code object (returns co_name as string constant)
            name_expr = stack.empty() ? nullptr : popExpr(stack);
        }

        std::string fname = "lambda";
        if (name_expr && name_expr->kind == PyExpr::Kind::Constant && !name_expr->sval.empty())
            fname = name_expr->sval;
        pushExpr(stack, makeName("__func_" + fname + "__"));
        return true;
    }

    // ── UNPACK_SEQUENCE ──────────────────────────────────────────────────────
    if (nm == "UNPACK_SEQUENCE") {
        auto seq = popExpr(stack);
        // Push individual elements (synthetic subscripts)
        for (int i = arg - 1; i >= 0; --i) {
            auto e = std::make_shared<PyExpr>();
            e->kind = PyExpr::Kind::Subscript;
            e->children = {seq, makeConst(static_cast<int64_t>(i))};
            pushExpr(stack, e);
        }
        return true;
    }
    if (nm == "UNPACK_EX") {
        auto seq = popExpr(stack);
        int before = arg & 0xFF;
        int after  = (arg >> 8) & 0xFF;
        // Push synthetic access expressions
        for (int i = before + after - 1; i >= 0; --i) {
            pushExpr(stack, makeName("_unpack_" + std::to_string(i) + "_"));
        }
        return true;
    }

    // ── GET_ITER / FOR_ITER ──────────────────────────────────────────────────
    if (nm == "GET_ITER") {
        // iter(TOS) — keep the expression, just mark it
        return true; // pass-through
    }

    // ── GLOBAL / NONLOCAL ────────────────────────────────────────────────────
    if (nm == "SETUP_ANNOTATIONS") {
        // __annotations__ = {}; not usually emitted in decompiled output
        return true;
    }

    // ── NOP / RESUME / CACHE ─────────────────────────────────────────────────
    if (nm == "NOP" || nm == "RESUME" || nm == "CACHE" ||
        nm == "PRECALL" ||
        nm == "COPY_FREE_VARS" || nm == "RETURN_GENERATOR") {
        return true;
    }
    // PUSH_NULL: in 3.11+ pushes a null-callable slot (consumed by CALL).
    if (nm == "PUSH_NULL") {
        pushExpr(stack, makeName("_null_"));
        return true;
    }

    // ── LIST/SET/MAP operations ───────────────────────────────────────────────
    if (nm == "LIST_APPEND" || nm == "SET_ADD") {
        popExpr(stack); // element
        return true;    // handled at CFG level (comprehension detection)
    }
    if (nm == "MAP_ADD") {
        popExpr(stack); popExpr(stack);
        return true;
    }
    if (nm == "LIST_EXTEND" || nm == "SET_UPDATE" ||
        nm == "DICT_MERGE" || nm == "DICT_UPDATE") {
        auto ext = popExpr(stack);
        // Add as starred/merge expression
        if (!stack.empty()) {
            auto& top = stack.back();
            if (top.expr && (top.expr->kind == PyExpr::Kind::List ||
                             top.expr->kind == PyExpr::Kind::Set ||
                             top.expr->kind == PyExpr::Kind::Dict)) {
                auto starred = std::make_shared<PyExpr>();
                starred->kind = PyExpr::Kind::Starred;
                starred->children = {ext};
                top.expr->values.push_back(starred);
            }
        }
        return true;
    }

    // ── PUSH_EXC_INFO / CHECK_EXC_MATCH / POP_EXCEPT ─────────────────────────
    if (nm == "PUSH_EXC_INFO") {
        auto exc = popExpr(stack);
        pushExpr(stack, exc); // keep
        pushExpr(stack, exc); // duplicate for handler
        return true;
    }
    if (nm == "CHECK_EXC_MATCH") {
        auto exc_type = popExpr(stack);
        auto exc_val  = stack.empty() ? makeName("_exc_") : stack.back().expr;
        auto cmp = std::make_shared<PyExpr>();
        cmp->kind = PyExpr::Kind::Compare;
        cmp->cmpOps = {CmpOp::Is}; // isinstance check placeholder
        cmp->children = {exc_val};
        cmp->values   = {exc_type};
        pushExpr(stack, cmp);
        return true;
    }
    if (nm == "POP_EXCEPT" || nm == "END_FINALLY" || nm == "POP_BLOCK") {
        return true;
    }

    // ── GET_AWAITABLE / AWAIT ────────────────────────────────────────────────
    if (nm == "GET_AWAITABLE") {
        auto coro = popExpr(stack);
        auto e = std::make_shared<PyExpr>();
        e->kind = PyExpr::Kind::Await;
        e->children = {coro};
        pushExpr(stack, e);
        return true;
    }

    // ── JUMP instructions: don't push/pop, handled at CFG level ──────────────
    if (nm.find("JUMP") != std::string::npos ||
        nm.find("POP_JUMP") != std::string::npos ||
        nm == "FOR_ITER" || nm == "SEND") {
        // POP_JUMP_IF_* pops condition
        if (nm.find("POP_JUMP") != std::string::npos && !stack.empty())
            popExpr(stack);
        return true;
    }

    // ── BEFORE_WITH / SETUP_WITH ─────────────────────────────────────────────
    if (nm == "BEFORE_WITH" || nm == "SETUP_WITH") {
        // context manager is on stack; push __exit__ and __enter__() result
        auto cm = popExpr(stack);
        pushExpr(stack, makeAttr(cm, "__exit__"));  // exit fn
        pushExpr(stack, makeCall(makeAttr(cm, "__enter__"), {})); // enter result
        return true;
    }
    if (nm == "WITH_EXCEPT_START") {
        // calls __exit__; result goes on stack
        pushExpr(stack, makeName("_with_exit_result_"));
        return true;
    }

    // ── MATCH opcodes ────────────────────────────────────────────────────────
    if (nm.rfind("MATCH_", 0) == 0) {
        // placeholder: push a name
        pushExpr(stack, makeName("_match_result_"));
        return true;
    }

    // ── Fallback ────────────────────────────────────────────────────────────
    warn("Unhandled opcode: " + nm);
    return true;
}

// ─── simulateBlock ────────────────────────────────────────────────────────────

BlockResult PyStackSimulator::simulateBlock(
        const std::vector<RawInstr>& instrs,
        size_t blockStart, size_t blockEnd,
        Stack entryStack) {
    BlockResult result;
    result.exitStack = entryStack;

    for (size_t i = blockStart; i < blockEnd; ++i) {
        applyInstr(instrs[i], result.exitStack, result.stmts);
        const auto& nm = instrs[i].name;
        if (nm == "RETURN_VALUE" || nm == "RETURN_CONST" ||
            nm == "RAISE_VARARGS" || nm == "RERAISE") {
            result.terminates = true;
            break;
        }
    }
    return result;
}

// ─── simulate ────────────────────────────────────────────────────────────────

StmtList PyStackSimulator::simulate() {
    auto instrs = decode();
    if (instrs.empty()) {
        StmtList empty;
        empty.push_back(makePass());
        return empty;
    }

    auto leaders = findLeaders(instrs);

    // Map offset → index in instrs
    std::unordered_map<uint32_t, size_t> offsetToIdx;
    for (size_t i = 0; i < instrs.size(); ++i)
        offsetToIdx[instrs[i].offset] = i;

    // Process blocks in order (topological = linear for well-formed bytecode)
    StmtList allStmts;
    Stack stack;

    for (size_t bi = 0; bi < leaders.size(); ++bi) {
        uint32_t blockStart  = leaders[bi];
        uint32_t blockEndOff = (bi + 1 < leaders.size()) ? leaders[bi+1] : UINT32_MAX;

        size_t startIdx = 0;
        auto it = offsetToIdx.find(blockStart);
        if (it != offsetToIdx.end()) startIdx = it->second;

        size_t endIdx = instrs.size();
        for (size_t i = startIdx; i < instrs.size(); ++i) {
            if (instrs[i].offset >= blockEndOff) { endIdx = i; break; }
        }

        auto result = simulateBlock(instrs, startIdx, endIdx, stack);
        for (auto& s : result.stmts) allStmts.push_back(s);
        if (!result.terminates)
            stack = result.exitStack;
        else
            stack.clear();
    }

    if (allStmts.empty())
        allStmts.push_back(makePass());

    return allStmts;
}

} // namespace py_reconstruct
} // namespace retdec
