/**
 * @file src/csharp_emitter/cs_expr_emitter.cpp
 * @brief CilExpr → C# expression string.
 */

#include "retdec/csharp_emitter/cs_expr_emitter.h"

#include <cassert>
#include <iomanip>
#include <sstream>
#include <variant>

namespace retdec {
namespace csharp_emitter {

// ─── CsExprEmitter ────────────────────────────────────────────────────────────

CsExprEmitter::CsExprEmitter(CsWriter& writer, Options opts)
    : writer_(writer), opts_(std::move(opts)) {}

// ─── Precedence / parenthesisation ───────────────────────────────────────────

std::string CsExprEmitter::parenIf(const std::string& text, int exprPrec, int parentPrec) {
    if (exprPrec < parentPrec) return "(" + text + ")";
    return text;
}

// ─── BinOpInfo ────────────────────────────────────────────────────────────────

std::pair<std::string, int> CsExprEmitter::binOpInfo(BinOpKind op) {
    switch (op) {
    case BinOpKind::Mul:    case BinOpKind::MulOvf:  case BinOpKind::MulOvfUn:
        return {"*",  12};
    case BinOpKind::Div:    case BinOpKind::DivUn:   return {"/",  12};
    case BinOpKind::Rem:    case BinOpKind::RemUn:   return {"%",  12};
    case BinOpKind::Add:    case BinOpKind::AddOvf:  case BinOpKind::AddOvfUn:
    case BinOpKind::PtrAdd:
        return {"+",  11};
    case BinOpKind::Sub:    case BinOpKind::SubOvf:  case BinOpKind::SubOvfUn:
        return {"-",  11};
    case BinOpKind::Shl:    return {"<<", 10};
    case BinOpKind::Shr:    return {">>", 10};
    case BinOpKind::ShrUn:  return {">>>",10};
    case BinOpKind::Lt:     case BinOpKind::LtUn:   return {"<",   9};
    case BinOpKind::Le:     case BinOpKind::LeUn:   return {"<=",  9};
    case BinOpKind::Gt:     case BinOpKind::GtUn:   return {">",   9};
    case BinOpKind::Ge:     case BinOpKind::GeUn:   return {">=",  9};
    case BinOpKind::Eq:     return {"==",  8};
    case BinOpKind::Ne:     return {"!=",  8};
    case BinOpKind::And:    return {"&",   7};
    case BinOpKind::Xor:    return {"^",   6};
    case BinOpKind::Or:     return {"|",   5};
    default:                return {"+",  11};
    }
}

// ─── UnOp text ────────────────────────────────────────────────────────────────

std::string CsExprEmitter::unOpText(UnOpKind op, const std::string& operand) {
    switch (op) {
    case UnOpKind::Neg:  return "-" + operand;
    case UnOpKind::Not:  return "~" + operand;
    case UnOpKind::ConvI1:  return "(sbyte)" + operand;
    case UnOpKind::ConvU1:  return "(byte)" + operand;
    case UnOpKind::ConvI2:  return "(short)" + operand;
    case UnOpKind::ConvU2:  return "(ushort)" + operand;
    case UnOpKind::ConvI4:  return "(int)" + operand;
    case UnOpKind::ConvU4:  return "(uint)" + operand;
    case UnOpKind::ConvI8:  return "(long)" + operand;
    case UnOpKind::ConvU8:  return "(ulong)" + operand;
    case UnOpKind::ConvR4:  return "(float)" + operand;
    case UnOpKind::ConvR8:  return "(double)" + operand;
    case UnOpKind::ConvI:   return "(nint)" + operand;
    case UnOpKind::ConvU:   return "(nuint)" + operand;
    case UnOpKind::ConvR_Un: return "(double)(uint)" + operand;
    // Checked conversions
    case UnOpKind::ConvOvfI1:  return "checked((sbyte)" + operand + ")";
    case UnOpKind::ConvOvfU1:  return "checked((byte)" + operand + ")";
    case UnOpKind::ConvOvfI2:  return "checked((short)" + operand + ")";
    case UnOpKind::ConvOvfU2:  return "checked((ushort)" + operand + ")";
    case UnOpKind::ConvOvfI4:  return "checked((int)" + operand + ")";
    case UnOpKind::ConvOvfU4:  return "checked((uint)" + operand + ")";
    case UnOpKind::ConvOvfI8:  return "checked((long)" + operand + ")";
    case UnOpKind::ConvOvfU8:  return "checked((ulong)" + operand + ")";
    case UnOpKind::ConvOvfI:   return "checked((nint)" + operand + ")";
    case UnOpKind::ConvOvfU:   return "checked((nuint)" + operand + ")";
    case UnOpKind::ConvOvfI1Un: return "checked((sbyte)(uint)" + operand + ")";
    case UnOpKind::ConvOvfU1Un: return "checked((byte)(uint)" + operand + ")";
    case UnOpKind::ConvOvfI2Un: return "checked((short)(uint)" + operand + ")";
    case UnOpKind::ConvOvfU2Un: return "checked((ushort)(uint)" + operand + ")";
    case UnOpKind::ConvOvfI4Un: return "checked((int)(uint)" + operand + ")";
    case UnOpKind::ConvOvfU4Un: return "checked((uint)(uint)" + operand + ")";
    case UnOpKind::ConvOvfI8Un: return "checked((long)(ulong)" + operand + ")";
    case UnOpKind::ConvOvfU8Un: return "checked((ulong)(ulong)" + operand + ")";
    case UnOpKind::ConvOvfIUn:  return "checked((nint)(nuint)" + operand + ")";
    case UnOpKind::ConvOvfUUn:  return "checked((nuint)(nuint)" + operand + ")";
    default: return operand;
    }
}

// ─── emitType ─────────────────────────────────────────────────────────────────

std::string CsExprEmitter::emitType(const BcType& type) const {
    // Use CLR name for proper C# aliases (bool, int, string, etc.)
    // clrName() returns "System.Int32", "System.Boolean", etc. which the alias map converts.
    // For reference types it also returns the FQN (e.g. "System.String").
    return shortenType(type.clrName());
}

std::string CsExprEmitter::shortenType(const std::string& fqn) const {
    return CsWriter::clrToCsharpType(fqn);
}

bool CsExprEmitter::isNullableValueType(const std::string& typeName) {
    return !typeName.empty() && typeName.back() == '?';
}

// ─── Leaf emitters ────────────────────────────────────────────────────────────

std::string CsExprEmitter::emitConst(const ExprConst& e) const {
    if (e.isString) return writer_.stringLiteral(e.strVal);
    if (e.isFloat) {
        std::ostringstream ss;
        ss << std::setprecision(17) << e.fltVal;
        std::string s = ss.str();
        // Ensure it parses as a double literal in C#
        if (s.find('.') == std::string::npos && s.find('e') == std::string::npos &&
            s.find('E') == std::string::npos && s.find('n') == std::string::npos)
            s += ".0";
        // Type suffix
        std::string tn = emitType(e.type);
        if (tn == "float") s += "f";
        return s;
    }
    // Integer constant
    std::string tn = emitType(e.type);
    if (tn == "bool") return (e.intVal != 0) ? "true" : "false";
    if (tn == "char") return CsWriter::charLiteral(static_cast<uint32_t>(e.intVal));

    std::string suffix;
    if (tn == "uint")   suffix = "u";
    else if (tn == "long")  suffix = "L";
    else if (tn == "ulong") suffix = "uL";

    return std::to_string(e.intVal) + suffix;
}

std::string CsExprEmitter::emitNull() const {
    return "null";
}

std::string CsExprEmitter::emitLocal(const ExprLocal& e) const {
    return CsWriter::safeName(e.name.empty() ? "loc" + std::to_string(e.idx) : e.name);
}

std::string CsExprEmitter::emitArg(const ExprArg& e) const {
    return CsWriter::safeName(e.name.empty() ? "arg" + std::to_string(e.idx) : e.name);
}

std::string CsExprEmitter::emitField(const ExprField& e, int prec) const {
    std::string obj;
    if (e.obj) obj = emit(e.obj, 14) + ".";
    return parenIf(obj + CsWriter::safeName(e.fieldName), 14, prec);
}

std::string CsExprEmitter::emitSField(const ExprSField& e) const {
    std::string cls = shortenType(e.className);
    return cls + "." + CsWriter::safeName(e.fieldName);
}

std::string CsExprEmitter::emitArgList(const std::vector<CilExprPtr>& args) const {
    std::string result;
    for (size_t i = 0; i < args.size(); ++i) {
        if (i > 0) result += ", ";
        result += emit(args[i]);
    }
    return result;
}

std::string CsExprEmitter::emitCall(const ExprCall& e, int prec) const {
    std::string prefix;
    if (e.obj) {
        prefix = emit(e.obj, 14) + ".";
    } else if (!e.className.empty()) {
        prefix = shortenType(e.className) + ".";
    }

    std::string call = prefix + CsWriter::safeName(e.methodName) +
                       "(" + emitArgList(e.args) + ")";
    return parenIf(call, 14, prec);
}

std::string CsExprEmitter::emitNewobj(const ExprNewobj& e) const {
    return "new " + shortenType(e.className) +
           "(" + emitArgList(e.args) + ")";
}

std::string CsExprEmitter::emitNewarr(const ExprNewarr& e) const {
    std::string elem = emitType(e.elemType);
    std::string len  = e.length ? emit(e.length) : "";
    return "new " + elem + "[" + len + "]";
}

std::string CsExprEmitter::emitLdelem(const ExprLdelem& e, int prec) const {
    std::string arr = emit(e.arr, 14);
    std::string idx = e.idx ? emit(e.idx) : "0";
    return parenIf(arr + "[" + idx + "]", 14, prec);
}

std::string CsExprEmitter::emitBinOp(const ExprBinOp& e, int prec) const {
    auto [tok, myPrec] = binOpInfo(e.op);
    std::string lhs = emit(e.lhs, myPrec);
    // Right-associative: rhs parens if same prec (for sub/div/rem correctness)
    std::string rhs = emit(e.rhs, myPrec + 1);
    return parenIf(lhs + " " + tok + " " + rhs, myPrec, prec);
}

std::string CsExprEmitter::emitUnOp(const ExprUnOp& e, int prec) const {
    std::string inner = emit(e.operand, 13);
    std::string text  = unOpText(e.op, inner);
    return parenIf(text, 13, prec);
}

std::string CsExprEmitter::emitCast(const ExprCast& e, int prec) const {
    std::string typeName = emitType(e.targetType);
    std::string inner    = emit(e.expr, 13);
    std::string text;
    if (e.isChecked)
        text = "checked((" + typeName + ")" + inner + ")";
    else
        text = "(" + typeName + ")" + inner;
    return parenIf(text, 13, prec);
}

std::string CsExprEmitter::emitIsinst(const ExprIsinst& e, int prec) const {
    std::string typeName = emitType(e.targetType);
    std::string inner    = emit(e.expr, 9);
    return parenIf(inner + " is " + typeName, 9, prec);
}

std::string CsExprEmitter::emitBox(const ExprBox& e, int prec) const {
    // Boxing is implicit in C#; just emit the inner expression
    return emit(e.expr, prec);
}

std::string CsExprEmitter::emitUnbox(const ExprUnbox& e, int prec) const {
    std::string typeName = emitType(e.targetType);
    std::string inner    = emit(e.expr, 13);
    return parenIf("(" + typeName + ")" + inner, 13, prec);
}

std::string CsExprEmitter::emitSizeof(const ExprSizeof& e) const {
    return "sizeof(" + emitType(e.ofType) + ")";
}

std::string CsExprEmitter::emitAddressOf(const ExprAddressOf& e, int prec) const {
    std::string inner = e.expr ? emit(e.expr, 13) : "null";
    return parenIf("&" + inner, 13, prec);
}

std::string CsExprEmitter::emitDeref(const ExprDeref& e, int prec) const {
    std::string inner = emit(e.addr, 13);
    return parenIf("*" + inner, 13, prec);
}

std::string CsExprEmitter::emitDup(const ExprDup& e, int prec) const {
    return e.expr ? emit(e.expr, prec) : "null";
}

std::string CsExprEmitter::emitLdToken(const ExprLdToken& e) const {
    return "typeof(" + e.tokenStr + ")";
}

std::string CsExprEmitter::emitLdFtn(const ExprLdFtn& e) const {
    // Emitted as a method group reference
    std::string cls = e.className.empty() ? "" : shortenType(e.className) + ".";
    return cls + CsWriter::safeName(e.methodName);
}

std::string CsExprEmitter::emitDefault(const ExprDefault& e) const {
    std::string tn = emitType(e.type);
    if (tn == "null" || tn == "object" || tn == "string") return "null";
    return "default(" + tn + ")";
}

std::string CsExprEmitter::emitTernary(const ExprTernary& e, int prec) const {
    std::string cond = emit(e.cond, 1);
    std::string then = emit(e.then, 1);
    std::string els  = emit(e.elseBr, 1);
    return parenIf(cond + " ? " + then + " : " + els, 1, prec);
}

std::string CsExprEmitter::emitLocAlloc(const ExprLocAlloc& e) const {
    std::string elem = emitType(e.elemType);
    std::string size = e.size ? emit(e.size) : "0";
    return "stackalloc " + elem + "[" + size + "]";
}

std::string CsExprEmitter::emitMkRefAny(const ExprMkRefAny& e, int prec) const {
    // __makeref(expr)
    std::string inner = emit(e.expr, 14);
    return "__makeref(" + inner + ")";
}

// ─── Top-level dispatch ───────────────────────────────────────────────────────

std::string CsExprEmitter::emit(const CilExprPtr& expr, int parentPrec) const {
    if (!expr) return "null";

    return std::visit([&](const auto& v) -> std::string {
        using T = std::decay_t<decltype(v)>;

        if constexpr (std::is_same_v<T, ExprConst>)     return emitConst(v);
        if constexpr (std::is_same_v<T, ExprNull>)      return emitNull();
        if constexpr (std::is_same_v<T, ExprLocal>)     return emitLocal(v);
        if constexpr (std::is_same_v<T, ExprArg>)       return emitArg(v);
        if constexpr (std::is_same_v<T, ExprField>)     return emitField(v, parentPrec);
        if constexpr (std::is_same_v<T, ExprSField>)    return emitSField(v);
        if constexpr (std::is_same_v<T, ExprCall>)      return emitCall(v, parentPrec);
        if constexpr (std::is_same_v<T, ExprNewobj>)    return emitNewobj(v);
        if constexpr (std::is_same_v<T, ExprNewarr>)    return emitNewarr(v);
        if constexpr (std::is_same_v<T, ExprLdelem>)    return emitLdelem(v, parentPrec);
        if constexpr (std::is_same_v<T, ExprBinOp>)     return emitBinOp(v, parentPrec);
        if constexpr (std::is_same_v<T, ExprUnOp>)      return emitUnOp(v, parentPrec);
        if constexpr (std::is_same_v<T, ExprCast>)      return emitCast(v, parentPrec);
        if constexpr (std::is_same_v<T, ExprIsinst>)    return emitIsinst(v, parentPrec);
        if constexpr (std::is_same_v<T, ExprBox>)       return emitBox(v, parentPrec);
        if constexpr (std::is_same_v<T, ExprUnbox>)     return emitUnbox(v, parentPrec);
        if constexpr (std::is_same_v<T, ExprSizeof>)    return emitSizeof(v);
        if constexpr (std::is_same_v<T, ExprAddressOf>) return emitAddressOf(v, parentPrec);
        if constexpr (std::is_same_v<T, ExprDeref>)     return emitDeref(v, parentPrec);
        if constexpr (std::is_same_v<T, ExprDup>)       return emitDup(v, parentPrec);
        if constexpr (std::is_same_v<T, ExprLdToken>)   return emitLdToken(v);
        if constexpr (std::is_same_v<T, ExprLdFtn>)     return emitLdFtn(v);
        if constexpr (std::is_same_v<T, ExprDefault>)   return emitDefault(v);
        if constexpr (std::is_same_v<T, ExprTernary>)   return emitTernary(v, parentPrec);
        if constexpr (std::is_same_v<T, ExprLocAlloc>)  return emitLocAlloc(v);
        if constexpr (std::is_same_v<T, ExprMkRefAny>)  return emitMkRefAny(v, parentPrec);
        if constexpr (std::is_same_v<T, ExprArgList>)   return "__arglist";
        if constexpr (std::is_same_v<T, ExprTypedRef>)
            return v.expr ? emit(v.expr, parentPrec) : "null";
        if constexpr (std::is_same_v<T, ExprRefAnyVal>)
            return "__refvalue(" + (v.expr ? emit(v.expr) : "null") + ", " + emitType(v.type) + ")";
        if constexpr (std::is_same_v<T, ExprRefAnyType>)
            return "__reftype(" + (v.expr ? emit(v.expr) : "null") + ")";

        return "/* unknown expr */";
    }, expr->data);
}

} // namespace csharp_emitter
} // namespace retdec
