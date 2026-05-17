/**
 * @file src/cxx_backend/cxx_ast.cpp
 * @brief C++ AST node toString() implementations.
 */

#include "retdec/cxx_backend/cxx_ast.h"

#include <sstream>

namespace retdec {
namespace cxx_backend {

// ─── CxxCastKind ─────────────────────────────────────────────────────────────

std::string cxxCastKindStr(CxxCastKind k) {
    switch (k) {
    case CxxCastKind::Static:       return "static_cast";
    case CxxCastKind::Reinterpret:  return "reinterpret_cast";
    case CxxCastKind::Dynamic:      return "dynamic_cast";
    case CxxCastKind::Const:        return "const_cast";
    }
    return "static_cast";
}

// ─── CxxNewExpr ──────────────────────────────────────────────────────────────

std::string CxxNewExpr::toString() const {
    codegen::Emitter em;
    std::string typeStr = allocType ? em.emitType(*allocType) : "void";
    std::string s = "new " + typeStr;
    if (isArray && arraySize) {
        s += "[" + em.emitExpr(*arraySize) + "]";
    } else {
        s += "(";
        for (size_t i = 0; i < args.size(); ++i) {
            if (i) s += ", ";
            s += em.emitExpr(*args[i]);
        }
        s += ")";
    }
    return s;
}

// ─── CxxDeleteExpr ───────────────────────────────────────────────────────────

std::string CxxDeleteExpr::toString() const {
    codegen::Emitter em;
    std::string ptrStr = ptr ? em.emitExpr(*ptr) : "nullptr";
    if (isArray) return "delete[] " + ptrStr;
    return "delete " + ptrStr;
}

// ─── CxxCastExpr ─────────────────────────────────────────────────────────────

std::string CxxCastExpr::toString() const {
    codegen::Emitter em;
    std::string ts = targetType ? em.emitType(*targetType) : "void*";
    std::string es = expr ? em.emitExpr(*expr) : "";
    return cxxCastKindStr(castKind) + "<" + ts + ">(" + es + ")";
}

// ─── CxxThrowExpr ────────────────────────────────────────────────────────────

std::string CxxThrowExpr::toString() const {
    if (!expr) return "throw";
    codegen::Emitter em;
    return "throw " + em.emitExpr(*expr);
}

// ─── CxxScopeExpr ────────────────────────────────────────────────────────────

std::string CxxScopeExpr::toString() const {
    std::string s;
    for (size_t i = 0; i < scopes.size(); ++i) {
        if (i) s += "::";
        s += scopes[i];
    }
    return s;
}

// ─── CxxMethodCallExpr ───────────────────────────────────────────────────────

std::string CxxMethodCallExpr::toString() const {
    codegen::Emitter em;
    std::string obj = object ? em.emitExpr(*object) : "";
    std::string op  = arrowAccess ? "->" : ".";
    std::string argStr;
    for (size_t i = 0; i < args.size(); ++i) {
        if (i) argStr += ", ";
        argStr += em.emitExpr(*args[i]);
    }
    return obj + op + methodName + "(" + argStr + ")";
}

// ─── CxxLambdaExpr ───────────────────────────────────────────────────────────

std::string CxxLambdaExpr::toString() const {
    std::string capture;
    switch (captureDefault) {
    case CaptureKind::ByValue: capture = "="; break;
    case CaptureKind::ByRef:   capture = "&"; break;
    default: break;
    }
    for (const auto& c : captureList) {
        if (!capture.empty()) capture += ", ";
        capture += c;
    }
    std::string params;
    for (size_t i = 0; i < this->params.size(); ++i) {
        if (i) params += ", ";
        codegen::Emitter em;
        params += em.emitType(*this->params[i].type) + " " + this->params[i].name;
    }
    return "[" + capture + "](" + params + ") { /* lambda body */ }";
}

// ─── CxxTryStmt ──────────────────────────────────────────────────────────────

std::string CxxTryStmt::toString(int indent, int indentWidth) const {
    auto ind = [&](int d) { return std::string(d * indentWidth, ' '); };
    std::string s = ind(indent) + "try {\n";
    // body would be emitted by CxxEmitter
    s += ind(indent) + "}\n";
    for (const auto& c : catches) {
        s += ind(indent) + "catch (";
        if (!c.exceptionType) {
            s += "...";
        } else {
            codegen::Emitter em;
            s += em.emitType(*c.exceptionType);
            if (!c.varName.empty()) s += " " + c.varName;
        }
        s += ") {\n";
        s += ind(indent) + "}\n";
    }
    return s;
}

// ─── CxxMethod ───────────────────────────────────────────────────────────────

std::string CxxMethod::toString(int indent, int indentWidth) const {
    auto ind = [&](int d) { return std::string(d * indentWidth, ' '); };
    codegen::Emitter em;

    std::string s = ind(indent);
    if (isVirtual)  s += "virtual ";
    if (isStatic)   s += "static ";
    if (isInline)   s += "inline ";

    if (isConstructor || isDestructor) {
        if (isDestructor) s += "~";
        s += name + "(";
    } else {
        s += em.emitType(*returnType) + " " + name + "(";
    }

    for (size_t i = 0; i < params.size(); ++i) {
        if (i) s += ", ";
        s += em.emitType(*params[i].type) + " " + params[i].name;
    }
    if (isVariadic) {
        if (!params.empty()) s += ", ";
        s += "...";
    }
    s += ")";
    if (isConst) s += " const";
    if (isOverride && !isPureVirtual) s += " override";
    if (isFinal)   s += " final";
    if (isPureVirtual) s += " = 0";

    if (!body) {
        s += ";";
    } else {
        s += " {\n";
        // body emitted externally
        s += ind(indent) + "}";
    }
    return s;
}

// ─── CxxClass ────────────────────────────────────────────────────────────────

bool CxxClass::isAbstract() const {
    for (const auto& m : methods)
        if (m.isPureVirtual) return true;
    return false;
}

std::string CxxClass::toString(int indent, int indentWidth) const {
    auto ind = [&](int d) { return std::string(d * indentWidth, ' '); };
    std::string s = ind(indent);
    s += (kind == Kind::Class ? "class " : "struct ") + name;
    if (!bases.empty()) {
        s += " : ";
        for (size_t i = 0; i < bases.size(); ++i) {
            if (i) s += ", ";
            if (!bases[i].access.empty()) s += bases[i].access + " ";
            if (bases[i].isVirtual) s += "virtual ";
            s += bases[i].name;
        }
    }
    s += " {\n";
    // Fields and methods would be emitted by CxxEmitter
    s += ind(indent) + "};\n";
    return s;
}

// ─── CxxNamespace ────────────────────────────────────────────────────────────

std::string CxxNamespace::toString() const {
    std::string s = "namespace";
    if (isInline) s = "inline namespace";
    if (!name.empty()) s += " " + name;
    s += " {\n" + contents + "\n}\n";
    return s;
}

// ─── CxxTemplate ─────────────────────────────────────────────────────────────

std::string CxxTemplate::paramStr() const {
    std::string s = "template<";
    for (size_t i = 0; i < params.size(); ++i) {
        if (i) s += ", ";
        if (params[i].kind == Param::Kind::Typename) {
            s += "typename " + params[i].name;
        } else {
            codegen::Emitter em;
            s += em.emitType(*params[i].type) + " " + params[i].name;
        }
        if (params[i].hasDefault) s += " = " + params[i].defaultStr;
    }
    s += ">";
    return s;
}

// ─── CxxEnum ─────────────────────────────────────────────────────────────────

std::string CxxEnum::toString() const {
    std::string s = "enum ";
    if (isClass) s += "class ";
    s += name;
    if (!underlying.empty()) s += " : " + underlying;
    s += " {\n";
    for (size_t i = 0; i < enumerators.size(); ++i) {
        s += "    " + enumerators[i].name;
        if (enumerators[i].value.has_value())
            s += " = " + std::to_string(*enumerators[i].value);
        if (i + 1 < enumerators.size()) s += ",";
        s += "\n";
    }
    s += "};\n";
    return s;
}

// ─── CxxUsing ────────────────────────────────────────────────────────────────

std::string CxxUsing::toString() const {
    if (isNamespace) return "using namespace " + name + ";";
    return "using " + name + " = " + target + ";";
}

} // namespace cxx_backend
} // namespace retdec
