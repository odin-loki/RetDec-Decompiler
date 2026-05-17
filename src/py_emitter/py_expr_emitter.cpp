/**
 * @file src/py_emitter/py_expr_emitter.cpp
 */

#include "retdec/py_emitter/py_expr_emitter.h"

#include <sstream>

namespace retdec {
namespace py_emitter {

PyExprEmitter::PyExprEmitter(PyWriter& writer) : writer_(writer) {}

// ─── Static helpers ───────────────────────────────────────────────────────────

std::string PyExprEmitter::binOpStr(BinOp op) {
    switch (op) {
    case BinOp::Add:      return "+";
    case BinOp::Sub:      return "-";
    case BinOp::Mult:     return "*";
    case BinOp::MatMult:  return "@";
    case BinOp::Div:      return "/";
    case BinOp::FloorDiv: return "//";
    case BinOp::Mod:      return "%";
    case BinOp::Pow:      return "**";
    case BinOp::LShift:   return "<<";
    case BinOp::RShift:   return ">>";
    case BinOp::BitOr:    return "|";
    case BinOp::BitXor:   return "^";
    case BinOp::BitAnd:   return "&";
    }
    return "+";
}

std::string PyExprEmitter::unOpStr(UnaryOp op) {
    switch (op) {
    case UnaryOp::Invert: return "~";
    case UnaryOp::Not:    return "not ";
    case UnaryOp::UAdd:   return "+";
    case UnaryOp::USub:   return "-";
    }
    return "";
}

std::string PyExprEmitter::cmpOpStr(CmpOp op) {
    switch (op) {
    case CmpOp::Eq:    return "==";
    case CmpOp::NotEq: return "!=";
    case CmpOp::Lt:    return "<";
    case CmpOp::LtE:   return "<=";
    case CmpOp::Gt:    return ">";
    case CmpOp::GtE:   return ">=";
    case CmpOp::Is:    return "is";
    case CmpOp::IsNot: return "is not";
    case CmpOp::In:    return "in";
    case CmpOp::NotIn: return "not in";
    }
    return "==";
}

PyPrec PyExprEmitter::binOpPrec(BinOp op) {
    switch (op) {
    case BinOp::BitOr:    return PyPrec::BitOr;
    case BinOp::BitXor:   return PyPrec::BitXor;
    case BinOp::BitAnd:   return PyPrec::BitAnd;
    case BinOp::LShift:
    case BinOp::RShift:   return PyPrec::Shift;
    case BinOp::Add:
    case BinOp::Sub:      return PyPrec::AddSub;
    case BinOp::Mult:
    case BinOp::MatMult:
    case BinOp::Div:
    case BinOp::FloorDiv:
    case BinOp::Mod:      return PyPrec::MulDiv;
    case BinOp::Pow:      return PyPrec::Pow;
    }
    return PyPrec::MulDiv;
}

std::string PyExprEmitter::parenIf(const std::string& s, PyPrec myPrec,
                                    PyPrec parentPrec) {
    if (static_cast<int>(myPrec) < static_cast<int>(parentPrec))
        return "(" + s + ")";
    return s;
}

// ─── emit ─────────────────────────────────────────────────────────────────────

std::string PyExprEmitter::emit(const PyExprPtr& expr, PyPrec parentPrec) const {
    if (!expr) return "None";

    switch (expr->kind) {
    case PyExpr::Kind::Constant:       return emitConst(*expr);
    case PyExpr::Kind::Name:           return PyWriter::safeName(expr->name);
    case PyExpr::Kind::BoolOp:         return emitBoolOp(*expr, parentPrec);
    case PyExpr::Kind::BinOp:          return emitBinOp(*expr, parentPrec);
    case PyExpr::Kind::UnaryOp:        return emitUnaryOp(*expr, parentPrec);
    case PyExpr::Kind::Compare:        return emitCompare(*expr, parentPrec);
    case PyExpr::Kind::Call:           return emitCall(*expr);
    case PyExpr::Kind::Attribute:      return emitAttr(*expr);
    case PyExpr::Kind::Subscript:      return emitSubscript(*expr);
    case PyExpr::Kind::Starred:        return emitStarred(*expr);
    case PyExpr::Kind::IfExp:          return emitIfExp(*expr, parentPrec);
    case PyExpr::Kind::Lambda:         return emitLambda(*expr);
    case PyExpr::Kind::JoinedStr:      return emitJoinedStr(*expr);
    case PyExpr::Kind::FormattedValue: return emitFormattedValue(*expr);
    case PyExpr::Kind::NamedExpr:      return emitNamedExpr(*expr);
    case PyExpr::Kind::ListComp:
    case PyExpr::Kind::SetComp:
    case PyExpr::Kind::DictComp:
    case PyExpr::Kind::GeneratorExp:   return emitComp(*expr);
    case PyExpr::Kind::Yield:
    case PyExpr::Kind::YieldFrom:      return emitYield(*expr);
    case PyExpr::Kind::Await:          return emitAwait(*expr);
    case PyExpr::Kind::List: {
        return "[" + emitList(expr->values) + "]";
    }
    case PyExpr::Kind::Tuple: {
        std::string s = emitList(expr->values);
        if (expr->values.size() == 1) s += ",";
        return "(" + s + ")";
    }
    case PyExpr::Kind::Set: {
        if (expr->values.empty()) return "set()";
        return "{" + emitList(expr->values) + "}";
    }
    case PyExpr::Kind::Dict: {
        std::ostringstream ss;
        ss << "{";
        for (size_t i = 0; i < expr->values.size(); ++i) {
            if (i) ss << ", ";
            if (i < expr->keys.size() && expr->keys[i]) {
                ss << emit(expr->keys[i]) << ": " << emit(expr->values[i]);
            } else {
                // **dict unpacking
                ss << "**" << emit(expr->values[i]);
            }
        }
        ss << "}";
        return ss.str();
    }
    }
    return "None";
}

// ─── emitConst ───────────────────────────────────────────────────────────────

std::string PyExprEmitter::emitConst(const PyExpr& e) const {
    switch (e.constKind) {
    case PyExpr::ConstKind::None_:     return "None";
    case PyExpr::ConstKind::True_:     return "True";
    case PyExpr::ConstKind::False_:    return "False";
    case PyExpr::ConstKind::Ellipsis_: return "...";
    case PyExpr::ConstKind::Int: {
        return std::to_string(e.ival);
    }
    case PyExpr::ConstKind::Float: {
        std::ostringstream ss;
        ss << e.fval;
        // Ensure it's a float literal (has decimal point)
        std::string s = ss.str();
        if (s.find('.') == std::string::npos && s.find('e') == std::string::npos)
            s += ".0";
        return s;
    }
    case PyExpr::ConstKind::Complex: {
        std::ostringstream ss;
        ss << e.fval << "+" << e.fval2 << "j";
        return ss.str();
    }
    case PyExpr::ConstKind::Str:
        return writer_.strLiteral(e.sval);
    case PyExpr::ConstKind::Bytes:
        return writer_.bytesLiteral(e.sval);
    }
    return "None";
}

// ─── emitBinOp ───────────────────────────────────────────────────────────────

std::string PyExprEmitter::emitBinOp(const PyExpr& e, PyPrec parentPrec) const {
    if (e.children.size() < 2) return "None";
    PyPrec myPrec = binOpPrec(e.binOp);
    // Pow is right-associative
    PyPrec leftPrec  = (e.binOp == BinOp::Pow) ? PyPrec::Unary : myPrec;
    PyPrec rightPrec = (e.binOp == BinOp::Pow) ? myPrec
                       : static_cast<PyPrec>(static_cast<int>(myPrec) + 1);
    std::string lhs = emit(e.children[0], leftPrec);
    std::string rhs = emit(e.children[1], rightPrec);
    std::string s   = lhs + " " + binOpStr(e.binOp) + " " + rhs;
    return parenIf(s, myPrec, parentPrec);
}

// ─── emitUnaryOp ─────────────────────────────────────────────────────────────

std::string PyExprEmitter::emitUnaryOp(const PyExpr& e, PyPrec parentPrec) const {
    if (e.children.empty()) return "None";
    std::string op  = unOpStr(e.unaryOp);
    std::string operand = emit(e.children[0], PyPrec::Unary);
    std::string s = op + operand;
    return parenIf(s, PyPrec::Unary, parentPrec);
}

// ─── emitBoolOp ──────────────────────────────────────────────────────────────

std::string PyExprEmitter::emitBoolOp(const PyExpr& e, PyPrec parentPrec) const {
    std::string op = (e.boolOp == BoolOp::And) ? " and " : " or ";
    PyPrec myPrec  = (e.boolOp == BoolOp::And) ? PyPrec::And : PyPrec::Or;
    std::string s;
    for (size_t i = 0; i < e.values.size(); ++i) {
        if (i) s += op;
        s += emit(e.values[i], static_cast<PyPrec>(static_cast<int>(myPrec)+1));
    }
    return parenIf(s, myPrec, parentPrec);
}

// ─── emitCompare ─────────────────────────────────────────────────────────────

std::string PyExprEmitter::emitCompare(const PyExpr& e, PyPrec parentPrec) const {
    if (e.children.empty()) return "None";
    std::string s = emit(e.children[0], PyPrec::Compare);
    for (size_t i = 0; i < e.cmpOps.size() && i < e.values.size(); ++i) {
        s += " " + cmpOpStr(e.cmpOps[i]) + " ";
        s += emit(e.values[i], PyPrec::Compare);
    }
    return parenIf(s, PyPrec::Compare, parentPrec);
}

// ─── emitCall ────────────────────────────────────────────────────────────────

std::string PyExprEmitter::emitCall(const PyExpr& e) const {
    std::string func = e.children.empty() ? "?" : emit(e.children[0], PyPrec::Atom);
    std::ostringstream ss;
    ss << func << "(";
    bool first = true;
    for (const auto& arg : e.values) {
        if (!first) ss << ", ";
        ss << emit(arg);
        first = false;
    }
    for (const auto& kw : e.keywords) {
        if (!first) ss << ", ";
        if (kw.arg.has_value())
            ss << *kw.arg << "=" << emit(kw.value);
        else
            ss << "**" << emit(kw.value);
        first = false;
    }
    ss << ")";
    return ss.str();
}

// ─── emitAttr ────────────────────────────────────────────────────────────────

std::string PyExprEmitter::emitAttr(const PyExpr& e) const {
    if (e.children.empty()) return "?." + e.attr;
    return emit(e.children[0], PyPrec::Atom) + "." + PyWriter::safeName(e.attr);
}

// ─── emitSubscript ───────────────────────────────────────────────────────────

std::string PyExprEmitter::emitSubscript(const PyExpr& e) const {
    if (e.isSlice) {
        std::string lower = e.sliceLower ? emit(e.sliceLower) : "";
        std::string upper = e.sliceUpper ? emit(e.sliceUpper) : "";
        std::string slice = lower + ":" + upper;
        if (e.sliceStep) slice += ":" + emit(e.sliceStep);
        if (e.children.empty()) return "[" + slice + "]";
        return emit(e.children[0], PyPrec::Atom) + "[" + slice + "]";
    }
    if (e.children.size() < 2) return "?[?]";
    return emit(e.children[0], PyPrec::Atom) + "[" + emit(e.children[1]) + "]";
}

// ─── emitStarred ─────────────────────────────────────────────────────────────

std::string PyExprEmitter::emitStarred(const PyExpr& e) const {
    if (e.children.empty()) return "*?";
    return "*" + emit(e.children[0], PyPrec::Atom);
}

// ─── emitIfExp ───────────────────────────────────────────────────────────────

std::string PyExprEmitter::emitIfExp(const PyExpr& e, PyPrec parentPrec) const {
    if (e.children.size() < 2 || e.values.empty()) return "None";
    // children[0]=body, values[0]=test, children[1]=orelse
    std::string body  = emit(e.children[0], PyPrec::IfExp);
    std::string test  = emit(e.values[0],   PyPrec::Or);
    std::string orelse= e.children.size()>1 ? emit(e.children[1], PyPrec::IfExp):"None";
    std::string s = body + " if " + test + " else " + orelse;
    return parenIf(s, PyPrec::IfExp, parentPrec);
}

// ─── emitLambda ──────────────────────────────────────────────────────────────

std::string PyExprEmitter::emitLambda(const PyExpr& e) const {
    std::string params = emitArguments(e.args_, false);
    std::string body   = e.children.empty() ? "None" : emit(e.children[0]);
    return "lambda " + params + ": " + body;
}

// ─── emitJoinedStr (f-string) ────────────────────────────────────────────────

std::string PyExprEmitter::emitJoinedStr(const PyExpr& e) const {
    std::ostringstream ss;
    ss << "f\"";
    for (const auto& part : e.values) {
        if (!part) continue;
        if (part->kind == PyExpr::Kind::Constant && part->constKind == PyExpr::ConstKind::Str) {
            // Raw string part inside f-string
            for (char c : part->sval) {
                if (c == '{') ss << "{{";
                else if (c == '}') ss << "}}";
                else ss << c;
            }
        } else if (part->kind == PyExpr::Kind::FormattedValue) {
            ss << "{" << emitFormattedValue(*part) << "}";
        } else {
            ss << "{" << emit(part) << "}";
        }
    }
    ss << "\"";
    return ss.str();
}

// ─── emitFormattedValue ──────────────────────────────────────────────────────

std::string PyExprEmitter::emitFormattedValue(const PyExpr& e) const {
    std::string val = e.children.empty() ? "?" : emit(e.children[0]);
    std::string s = val;
    if (e.conversion == 's') s += "!s";
    else if (e.conversion == 'r') s += "!r";
    else if (e.conversion == 'a') s += "!a";
    if (e.format_spec) s += ":" + emit(e.format_spec);
    return s;
}

// ─── emitNamedExpr ───────────────────────────────────────────────────────────

std::string PyExprEmitter::emitNamedExpr(const PyExpr& e) const {
    if (e.children.size() < 2) return "?:=?";
    return "(" + emit(e.children[0]) + " := " + emit(e.children[1]) + ")";
}

// ─── emitComp ────────────────────────────────────────────────────────────────

std::string PyExprEmitter::emitComp(const PyExpr& e) const {
    const char* open = "["; const char* close = "]";
    if (e.kind == PyExpr::Kind::SetComp)  { open = "{"; close = "}"; }
    if (e.kind == PyExpr::Kind::DictComp) { open = "{"; close = "}"; }
    if (e.kind == PyExpr::Kind::GeneratorExp) { open = "("; close = ")"; }

    std::string elt = e.children.empty() ? "?" : emit(e.children[0]);
    if (e.kind == PyExpr::Kind::DictComp) {
        elt += ": " + (e.elt2 ? emit(e.elt2) : "?");
    }

    std::string body = elt;
    for (const auto& gen : e.generators) {
        std::string prefix = gen.isAsync ? " async for " : " for ";
        body += prefix + emit(gen.target) + " in " + emit(gen.iter);
        for (const auto& cond : gen.ifs)
            body += " if " + emit(cond);
    }
    return open + body + close;
}

// ─── emitYield ───────────────────────────────────────────────────────────────

std::string PyExprEmitter::emitYield(const PyExpr& e) const {
    if (e.kind == PyExpr::Kind::YieldFrom) {
        return "yield from " + (e.children.empty() ? "?" : emit(e.children[0]));
    }
    if (e.children.empty() || !e.children[0]) return "yield";
    return "yield " + emit(e.children[0]);
}

// ─── emitAwait ───────────────────────────────────────────────────────────────

std::string PyExprEmitter::emitAwait(const PyExpr& e) const {
    if (e.children.empty()) return "await ?";
    return "await " + emit(e.children[0], PyPrec::Atom);
}

// ─── emitList ────────────────────────────────────────────────────────────────

std::string PyExprEmitter::emitList(const ExprList& exprs, const std::string& sep) const {
    std::string s;
    for (size_t i = 0; i < exprs.size(); ++i) {
        if (i) s += sep;
        s += emit(exprs[i]);
    }
    return s;
}

// ─── emitAnnotation ──────────────────────────────────────────────────────────

std::string PyExprEmitter::emitAnnotation(const PyExprPtr& ann) const {
    return emit(ann);
}

// ─── emitKeywords ────────────────────────────────────────────────────────────

std::string PyExprEmitter::emitKeywords(const std::vector<PyKeyword>& kws) const {
    std::string s;
    for (const auto& kw : kws) {
        if (!s.empty()) s += ", ";
        if (kw.arg.has_value())
            s += *kw.arg + "=" + emit(kw.value);
        else
            s += "**" + emit(kw.value);
    }
    return s;
}

// ─── emitArguments ───────────────────────────────────────────────────────────

std::string PyExprEmitter::emitArguments(const PyArguments& args, bool inDef) const {
    (void)inDef;
    std::string s;
    bool first = true;
    auto addArg = [&](const PyArgPtr& a, const std::string& prefix = "") {
        if (!first) s += ", ";
        s += prefix + PyWriter::safeName(a->arg);
        if (a->annotation) s += ": " + emit(a->annotation);
        first = false;
    };

    for (const auto& a : args.posonlyargs) addArg(a);
    if (!args.posonlyargs.empty()) {
        if (!first) s += ", ";
        s += "/";
        first = false;
    }
    for (const auto& a : args.args) addArg(a);

    if (args.vararg.has_value()) addArg(*args.vararg, "*");
    else if (!args.kwonlyargs.empty()) {
        if (!first) s += ", ";
        s += "*";
        first = false;
    }
    for (const auto& a : args.kwonlyargs) addArg(a);
    if (args.kwarg.has_value()) addArg(*args.kwarg, "**");
    return s;
}

} // namespace py_emitter
} // namespace retdec
