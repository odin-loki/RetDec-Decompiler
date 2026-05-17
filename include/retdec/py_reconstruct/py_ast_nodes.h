/**
 * @file include/retdec/py_reconstruct/py_ast_nodes.h
 * @brief Python AST node types for decompiled Python source.
 *
 * This mirrors the CPython ast module hierarchy (PEP 619 / ast.py) closely
 * so that the emitter can produce correct Python source for each node type.
 *
 * Design:
 *   - All nodes are heap-allocated and managed through shared_ptr.
 *   - Every node carries a line_no / col_offset pair for source attribution.
 *   - The visitor pattern (PyAstVisitor) allows the emitter to dispatch
 *     without runtime type checks.
 *
 * Subset implemented: everything needed to represent the output of CPython
 * 3.8-3.12 decompilation (no full Python 3.13 features needed yet).
 */

#ifndef RETDEC_PY_RECONSTRUCT_PY_AST_NODES_H
#define RETDEC_PY_RECONSTRUCT_PY_AST_NODES_H

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace retdec {
namespace py_reconstruct {

// ─── Forward declarations ─────────────────────────────────────────────────────

struct PyExpr;
struct PyStmt;
struct PyArg;
struct PyAlias;
struct PyKeyword;
struct PyComprehension;
struct PyExceptHandler;
struct PyMatchCase;
struct PyPattern;

using PyExprPtr  = std::shared_ptr<PyExpr>;
using PyStmtPtr  = std::shared_ptr<PyStmt>;
using PyArgPtr   = std::shared_ptr<PyArg>;
using StmtList   = std::vector<PyStmtPtr>;
using ExprList   = std::vector<PyExprPtr>;

// ─── Source location ─────────────────────────────────────────────────────────

struct SrcLoc {
    int line   = 0;
    int col    = 0;
    int endLine= 0;
    int endCol = 0;
};

// ─── Operator enums ──────────────────────────────────────────────────────────

enum class BoolOp  { And, Or };
enum class BinOp   {
    Add, Sub, Mult, MatMult, Div, Mod, Pow,
    LShift, RShift, BitOr, BitXor, BitAnd, FloorDiv
};
enum class UnaryOp { Invert, Not, UAdd, USub };
enum class CmpOp   { Eq, NotEq, Lt, LtE, Gt, GtE, Is, IsNot, In, NotIn };
enum class AugOp   { // same as BinOp but for augmented assignment
    Add, Sub, Mult, MatMult, Div, Mod, Pow,
    LShift, RShift, BitOr, BitXor, BitAnd, FloorDiv
};

// ─── Expression context ───────────────────────────────────────────────────────

enum class ExprCtx { Load, Store, Del };

// ─── PyArg (function argument with optional annotation and default) ───────────

struct PyArg {
    std::string  arg;                    // parameter name
    PyExprPtr    annotation;             // type hint (may be null)
    SrcLoc       loc;
};

// ─── PyKeyword (keyword=value in call) ───────────────────────────────────────

struct PyKeyword {
    std::optional<std::string> arg;     // None → **kwargs
    PyExprPtr                  value;
};

// ─── PyAlias (import alias) ──────────────────────────────────────────────────

struct PyAlias {
    std::string              name;
    std::optional<std::string> asname;
};

// ─── PyComprehension ─────────────────────────────────────────────────────────

struct PyComprehension {
    PyExprPtr        target;
    PyExprPtr        iter;
    ExprList         ifs;
    bool             isAsync = false;
};

// ─── PyExceptHandler ─────────────────────────────────────────────────────────

struct PyExceptHandler {
    PyExprPtr                   type;   // null → bare except
    std::optional<std::string>  name;   // "as e"
    StmtList                    body;
    SrcLoc                      loc;
};

// ─── PyPattern (match/case patterns, Python 3.10+) ───────────────────────────

struct PyPattern {
    enum class Kind {
        MatchValue, MatchSingleton, MatchSequence, MatchMapping,
        MatchClass, MatchStar, MatchAs, MatchOr
    };
    Kind    kind = Kind::MatchValue;
    PyExprPtr value;
    std::vector<std::shared_ptr<PyPattern>> patterns;
    std::optional<std::string> name;
    ExprList keys;
    ExprList cls_patterns_keys;  // for MatchClass keyword keys
};

// ─── PyMatchCase ─────────────────────────────────────────────────────────────

struct PyMatchCase {
    std::shared_ptr<PyPattern>  pattern;
    PyExprPtr                   guard;   // null → no guard
    StmtList                    body;
};

// ─── PyArguments (full argument list for a function) ──────────────────────────

struct PyArguments {
    std::vector<PyArgPtr> posonlyargs;  // positional-only (before /)
    std::vector<PyArgPtr> args;
    std::optional<PyArgPtr> vararg;     // *args
    std::vector<PyArgPtr> kwonlyargs;
    ExprList              kw_defaults;  // parallel to kwonlyargs (null = no default)
    std::optional<PyArgPtr> kwarg;      // **kwargs
    ExprList              defaults;     // last N of args have defaults
};

// ─── PyExpr ──────────────────────────────────────────────────────────────────

struct PyExpr {
    SrcLoc loc;

    enum class Kind {
        // Literals
        Constant,
        // Collections
        Tuple, List, Set, Dict,
        // Names
        Name,
        // Attribute / subscript / starred
        Attribute, Subscript, Starred,
        // Comprehensions
        ListComp, SetComp, DictComp, GeneratorExp,
        // Operators
        BoolOp, BinOp, UnaryOp, Compare,
        // Conditionals
        IfExp,
        // Lambda
        Lambda,
        // Calls
        Call,
        // Yield
        Yield, YieldFrom, Await,
        // F-string
        JoinedStr, FormattedValue,
        // Walrus
        NamedExpr,
    };

    Kind kind;

    // ── Constant ──────────────────────────────────────────────────────────────
    // value encoding: bool → bool in ival, int → ival, float → fval,
    // str → sval, bytes → sval (raw), None/True/False/Ellipsis → kind + sub
    enum class ConstKind { None_, True_, False_, Ellipsis_, Int, Float, Complex, Str, Bytes };
    ConstKind    constKind = ConstKind::None_;
    int64_t      ival   = 0;
    double       fval   = 0.0;
    double       fval2  = 0.0; // complex imag part
    std::string  sval;

    // ── Name ──────────────────────────────────────────────────────────────────
    std::string  name;
    ExprCtx      ctx = ExprCtx::Load;

    // ── BoolOp: values joined by op ──────────────────────────────────────────
    BoolOp       boolOp = BoolOp::And;

    // ── BinOp ─────────────────────────────────────────────────────────────────
    BinOp        binOp = BinOp::Add;

    // ── UnaryOp ───────────────────────────────────────────────────────────────
    UnaryOp      unaryOp = UnaryOp::Not;

    // ── Compare ───────────────────────────────────────────────────────────────
    std::vector<CmpOp> cmpOps;

    // ── Attribute ─────────────────────────────────────────────────────────────
    std::string  attr;

    // ── Subscript slice ───────────────────────────────────────────────────────
    // For Slice: lower/upper/step may be null
    bool         isSlice    = false;
    PyExprPtr    sliceLower;
    PyExprPtr    sliceUpper;
    PyExprPtr    sliceStep;

    // ── Call ──────────────────────────────────────────────────────────────────
    std::vector<PyKeyword>  keywords;

    // ── Lambda ────────────────────────────────────────────────────────────────
    PyArguments  args_;

    // ── Dict ──────────────────────────────────────────────────────────────────
    // keys[i] = null → **val (dict unpacking)
    ExprList     keys;     // also used for compare comparators

    // ── Comprehensions ────────────────────────────────────────────────────────
    std::vector<PyComprehension> generators;
    // DictComp: elt = key, elt2 = value
    PyExprPtr    elt2;

    // ── FormattedValue (inside f-string) ─────────────────────────────────────
    int          conversion = -1;  // -1=none, 's'=!s, 'r'=!r, 'a'=!a
    PyExprPtr    format_spec; // null if no format spec

    // ── NamedExpr (walrus :=) ─────────────────────────────────────────────────
    // target = Name node, value = value expr (stored in children[0], children[1])

    // ── Generic child storage ─────────────────────────────────────────────────
    ExprList     children;  // left/right/operand/value/target as needed
    ExprList     values;    // for BoolOp values, Compare comparators, List/Tuple elts
};

// ─── PyStmt ──────────────────────────────────────────────────────────────────

struct PyStmt {
    SrcLoc loc;

    enum class Kind {
        // Simple statements
        Expr,           // expression statement
        Assign,         // a = b = expr
        AugAssign,      // a += expr
        AnnAssign,      // a: T = expr
        Delete,
        Return,
        Yield,          // bare yield (as statement, wraps yield expr)
        Raise,
        Assert,
        Pass,
        Break,
        Continue,
        Global,
        Nonlocal,
        Import,
        ImportFrom,
        // Compound statements
        If,
        For,
        AsyncFor,
        While,
        With,
        AsyncWith,
        FunctionDef,
        AsyncFunctionDef,
        ClassDef,
        Try,
        TryStar,        // except* (3.11+)
        Match,          // match/case (3.10+)
    };

    Kind kind;

    // ── Expr / Return / Delete / Yield / Raise / Assert ───────────────────────
    PyExprPtr expr;         // main expression
    PyExprPtr expr2;        // second (raise cause, assert msg, annassign value)
    PyExprPtr expr3;        // for annotation in AnnAssign

    // ── Assign ────────────────────────────────────────────────────────────────
    ExprList  targets;
    // value in expr

    // ── AugAssign ─────────────────────────────────────────────────────────────
    AugOp     augOp = AugOp::Add;
    // target in targets[0], value in expr

    // ── AnnAssign ─────────────────────────────────────────────────────────────
    bool      simple = true;      // annotation is a simple name (not subscript)

    // ── Global / Nonlocal ─────────────────────────────────────────────────────
    std::vector<std::string> names;

    // ── Import / ImportFrom ───────────────────────────────────────────────────
    std::vector<PyAlias>     aliases;
    std::optional<std::string> module;
    int                      level = 0; // dots for relative import

    // ── If / While / For ──────────────────────────────────────────────────────
    StmtList  body;
    StmtList  orelse;   // elif/else/for-else/while-else

    // ── For ───────────────────────────────────────────────────────────────────
    PyExprPtr forTarget;
    PyExprPtr forIter;
    bool      isAsync = false;

    // ── With ──────────────────────────────────────────────────────────────────
    struct WithItem {
        PyExprPtr context_expr;
        PyExprPtr optional_vars;  // as target (null if none)
    };
    std::vector<WithItem> withItems;

    // ── FunctionDef / AsyncFunctionDef ────────────────────────────────────────
    std::string                  funcName;
    PyArguments                  funcArgs;
    PyExprPtr                    returnAnnotation;
    ExprList                     decorators;
    std::vector<std::string>     typeParams; // PEP 695 3.12+

    // ── ClassDef ──────────────────────────────────────────────────────────────
    std::string  className;
    ExprList     bases;
    std::vector<PyKeyword>  classKeywords; // metaclass=...
    ExprList     classDecorators;

    // ── Try ───────────────────────────────────────────────────────────────────
    std::vector<PyExceptHandler>  handlers;
    StmtList                      finalbody;

    // ── Match ─────────────────────────────────────────────────────────────────
    std::vector<PyMatchCase>      cases;
};

// ─── PyModule ────────────────────────────────────────────────────────────────

struct PyModule {
    StmtList   body;
    std::string filename;
    int        pythonMajor = 3;
    int        pythonMinor = 10;
};

// ─── Factory helpers ─────────────────────────────────────────────────────────

inline PyExprPtr makeConst(int64_t v, SrcLoc loc = SrcLoc{}) {
    auto e = std::make_shared<PyExpr>();
    e->kind = PyExpr::Kind::Constant;
    e->constKind = PyExpr::ConstKind::Int;
    e->ival = v;
    e->loc  = loc;
    return e;
}

inline PyExprPtr makeConst(double v, SrcLoc loc = SrcLoc{}) {
    auto e = std::make_shared<PyExpr>();
    e->kind = PyExpr::Kind::Constant;
    e->constKind = PyExpr::ConstKind::Float;
    e->fval = v;
    e->loc  = loc;
    return e;
}

inline PyExprPtr makeConst(const std::string& s, bool isBytes, SrcLoc loc = SrcLoc{}) {
    auto e = std::make_shared<PyExpr>();
    e->kind = PyExpr::Kind::Constant;
    e->constKind = isBytes ? PyExpr::ConstKind::Bytes : PyExpr::ConstKind::Str;
    e->sval = s;
    e->loc  = loc;
    return e;
}

inline PyExprPtr makeNone(SrcLoc loc = SrcLoc{}) {
    auto e = std::make_shared<PyExpr>();
    e->kind = PyExpr::Kind::Constant;
    e->constKind = PyExpr::ConstKind::None_;
    e->loc  = loc;
    return e;
}

inline PyExprPtr makeName(const std::string& n, ExprCtx ctx = ExprCtx::Load,
                           SrcLoc loc = SrcLoc{}) {
    auto e = std::make_shared<PyExpr>();
    e->kind = PyExpr::Kind::Name;
    e->name = n;
    e->ctx  = ctx;
    e->loc  = loc;
    return e;
}

inline PyExprPtr makeCall(PyExprPtr func, ExprList args, SrcLoc loc = SrcLoc{}) {
    auto e = std::make_shared<PyExpr>();
    e->kind = PyExpr::Kind::Call;
    e->children = {std::move(func)};
    e->values   = std::move(args);
    e->loc = loc;
    return e;
}

inline PyExprPtr makeAttr(PyExprPtr obj, const std::string& attr,
                           ExprCtx ctx = ExprCtx::Load, SrcLoc loc = SrcLoc{}) {
    auto e = std::make_shared<PyExpr>();
    e->kind = PyExpr::Kind::Attribute;
    e->children = {std::move(obj)};
    e->attr = attr;
    e->ctx  = ctx;
    e->loc  = loc;
    return e;
}

inline PyStmtPtr makeReturn(PyExprPtr val, SrcLoc loc = SrcLoc{}) {
    auto s = std::make_shared<PyStmt>();
    s->kind = PyStmt::Kind::Return;
    s->expr = std::move(val);
    s->loc  = loc;
    return s;
}

inline PyStmtPtr makeExprStmt(PyExprPtr e, SrcLoc loc = SrcLoc{}) {
    auto s = std::make_shared<PyStmt>();
    s->kind = PyStmt::Kind::Expr;
    s->expr = std::move(e);
    s->loc  = loc;
    return s;
}

inline PyStmtPtr makeAssign(ExprList targets, PyExprPtr value, SrcLoc loc = SrcLoc{}) {
    auto s = std::make_shared<PyStmt>();
    s->kind    = PyStmt::Kind::Assign;
    s->targets = std::move(targets);
    s->expr    = std::move(value);
    s->loc     = loc;
    return s;
}

inline PyStmtPtr makePass(SrcLoc loc = SrcLoc{}) {
    auto s = std::make_shared<PyStmt>();
    s->kind = PyStmt::Kind::Pass;
    s->loc  = loc;
    return s;
}

} // namespace py_reconstruct
} // namespace retdec

#endif // RETDEC_PY_RECONSTRUCT_PY_AST_NODES_H
