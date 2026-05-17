/**
 * @file src/cxx_backend/cxx_emitter.cpp
 * @brief C++ source emitter implementation.
 */

#include "retdec/cxx_backend/cxx_emitter.h"

#include <sstream>
#include <unordered_map>

namespace retdec {
namespace cxx_backend {

// ─── Construction ─────────────────────────────────────────────────────────────

CxxEmitter::CxxEmitter(CxxEmitConfig cfg) : cfg_(std::move(cfg)) {}

// ─── Helpers ─────────────────────────────────────────────────────────────────

std::string CxxEmitter::ind(int level) const {
    return std::string(level * cfg_.indentWidth, ' ');
}

std::string CxxEmitter::openBrace(int indent, bool sameLine) const {
    if (sameLine) return " {\n";
    return "\n" + ind(indent) + "{\n";
}

std::string CxxEmitter::closeBrace(int indent) const {
    return ind(indent) + "}";
}

// ─── Delegate to plain emitter ───────────────────────────────────────────────

std::string CxxEmitter::emitType(const CType& t, const std::string& name) const {
    return plain_.emitType(t, name);
}

std::string CxxEmitter::emitExpr(const CExpr& e, int outerPrec) const {
    return plain_.emitExpr(e, outerPrec);
}

std::string CxxEmitter::emitStmt(const CStmt& s, int indent) const {
    codegen::Emitter::Config cfg;
    cfg.indentWidth = cfg_.indentWidth;
    cfg.krBraces    = cfg_.krBraces;
    cfg.declsAtTop  = cfg_.declsAtTop;
    return plain_.emitStmt(s, indent, cfg);
}

std::string CxxEmitter::emitFunction(const codegen::CFunction& fn,
                                       int indent) const {
    codegen::Emitter::Config cfg;
    cfg.indentWidth = cfg_.indentWidth;
    cfg.krBraces    = cfg_.krBraces;
    return plain_.emitFunction(fn, cfg);
}

// ─── C++ expression emitters ─────────────────────────────────────────────────

std::string CxxEmitter::emitNew(const CxxNewExpr& n) const {
    return n.toString();
}

std::string CxxEmitter::emitDelete(const CxxDeleteExpr& d) const {
    return d.toString();
}

std::string CxxEmitter::emitCxxCast(const CxxCastExpr& c) const {
    return c.toString();
}

std::string CxxEmitter::emitThrow(const CxxThrowExpr& t) const {
    return t.toString();
}

std::string CxxEmitter::emitScope(const CxxScopeExpr& s) const {
    return s.toString();
}

std::string CxxEmitter::emitMethodCall(const CxxMethodCallExpr& mc) const {
    return mc.toString();
}

std::string CxxEmitter::emitLambda(const CxxLambdaExpr& lam, int indent) const {
    return lam.toString();
}

// ─── using ───────────────────────────────────────────────────────────────────

std::string CxxEmitter::emitUsing(const CxxUsing& u) const {
    return u.toString() + "\n";
}

// ─── namespace ───────────────────────────────────────────────────────────────

std::string CxxEmitter::emitNamespace(const CxxNamespace& ns, int indent) const {
    std::string s = ind(indent);
    if (ns.isInline) s += "inline ";
    s += "namespace";
    if (!ns.name.empty()) s += " " + ns.name;
    s += openBrace(indent, cfg_.krBraces);
    if (!ns.contents.empty()) s += ns.contents;
    s += closeBrace(indent) + "\n";
    return s;
}

// ─── template ────────────────────────────────────────────────────────────────

std::string CxxEmitter::emitTemplate(const CxxTemplate& tmpl) const {
    return tmpl.paramStr() + "\n";
}

// ─── enum class ──────────────────────────────────────────────────────────────

std::string CxxEmitter::emitEnum(const CxxEnum& en, int indent) const {
    std::string s = ind(indent) + "enum ";
    if (en.isClass) s += "class ";
    s += en.name;
    if (!en.underlying.empty()) s += " : " + en.underlying;
    s += openBrace(indent, cfg_.krBraces);
    for (size_t i = 0; i < en.enumerators.size(); ++i) {
        s += ind(indent + 1) + en.enumerators[i].name;
        if (en.enumerators[i].value.has_value())
            s += " = " + std::to_string(*en.enumerators[i].value);
        if (i + 1 < en.enumerators.size()) s += ",";
        s += "\n";
    }
    s += closeBrace(indent) + ";\n";
    return s;
}

// ─── try / catch ─────────────────────────────────────────────────────────────

std::string CxxEmitter::emitTry(const CxxTryStmt& t, int indent) const {
    std::string s = ind(indent) + "try" + openBrace(indent, cfg_.krBraces);
    if (t.tryBody)
        s += emitStmt(*t.tryBody, indent + 1);
    s += closeBrace(indent) + "\n";

    for (const auto& c : t.catches) {
        s += ind(indent) + "catch (";
        if (!c.exceptionType) {
            s += "...";
        } else {
            s += emitType(*c.exceptionType);
            if (!c.varName.empty()) s += " " + c.varName;
        }
        s += ")" + openBrace(indent, cfg_.krBraces);
        if (c.body) s += emitStmt(*c.body, indent + 1);
        s += closeBrace(indent) + "\n";
    }
    return s;
}

// ─── Method ──────────────────────────────────────────────────────────────────

std::string CxxEmitter::emitMethod(const CxxMethod& m, int indent,
                                     const std::string& className) const {
    std::string s = ind(indent);

    if (m.isVirtual)  s += "virtual ";
    if (m.isStatic)   s += "static ";
    if (m.isInline)   s += "inline ";

    // Return type (skip for ctor/dtor)
    if (!m.isConstructor && !m.isDestructor) {
        if (m.returnType) s += emitType(*m.returnType) + " ";
        else              s += "void ";
    }

    // Qualified name when emitting outside class body
    if (!className.empty() && !m.isConstructor && !m.isDestructor)
        s += className + "::";
    if (m.isDestructor) {
        if (!className.empty()) s += className + "::";
        s += "~" + m.name;
    } else {
        s += m.name;
    }

    // Parameters
    s += "(";
    for (size_t i = 0; i < m.params.size(); ++i) {
        if (i) s += ", ";
        if (m.params[i].type)
            s += emitType(*m.params[i].type) + " " + m.params[i].name;
        else
            s += m.params[i].name;
    }
    if (m.isVariadic) {
        if (!m.params.empty()) s += ", ";
        s += "...";
    }
    s += ")";

    if (m.isConst) s += " const";
    if (cfg_.emitOverride && m.isOverride && !m.isPureVirtual) s += " override";
    if (m.isFinal)        s += " final";
    if (m.isPureVirtual)  s += " = 0";

    if (!m.body) {
        s += ";\n";
    } else {
        s += openBrace(indent, cfg_.krBraces);
        s += emitStmt(*m.body, indent + 1);
        s += closeBrace(indent) + "\n";
    }
    return s;
}

// ─── Field list helpers ───────────────────────────────────────────────────────

std::string CxxEmitter::emitFieldList(
        const std::vector<CxxClass::Field>& fields,
        const std::string& access, int indent) const {
    std::string s;
    bool first = true;
    for (const auto& f : fields) {
        if (f.access != access) continue;
        if (first) {
            s += ind(indent - 1) + access + ":\n";
            first = false;
        }
        s += ind(indent);
        if (f.isStatic) s += "static ";
        if (f.type) s += emitType(*f.type) + " " + f.name;
        else        s += f.name;
        if (f.initValue) s += " = " + emitExpr(*f.initValue);
        s += ";\n";
    }
    return s;
}

std::string CxxEmitter::emitMethodList(
        const std::vector<CxxMethod>& methods,
        const std::string& access, int indent,
        const std::string& className) const {
    std::string s;
    bool first = true;
    for (const auto& m : methods) {
        // Simple access heuristic: public by default, private if starts with _
        std::string mAccess = (m.name.size() > 0 && m.name[0] == '_')
                               ? "private" : "public";
        if (m.isConstructor || m.isDestructor) mAccess = "public";
        if (mAccess != access) continue;
        if (first) {
            s += ind(indent - 1) + access + ":\n";
            first = false;
        }
        s += emitMethod(m, indent, "");
    }
    return s;
}

// ─── Class ───────────────────────────────────────────────────────────────────

std::string CxxEmitter::emitClass(const CxxClass& cls, int indent) const {
    std::string s = ind(indent);
    s += (cls.kind == CxxClass::Kind::Class ? "class " : "struct ") + cls.name;

    if (!cls.bases.empty()) {
        s += " :";
        for (size_t i = 0; i < cls.bases.size(); ++i) {
            s += (i == 0 ? " " : ", ");
            if (!cls.bases[i].access.empty()) s += cls.bases[i].access + " ";
            if (cls.bases[i].isVirtual) s += "virtual ";
            s += cls.bases[i].name;
        }
    }
    s += openBrace(indent, cfg_.krBraces);

    // Emit fields and methods grouped by access
    for (const char* acc : {"public", "protected", "private"}) {
        s += emitFieldList(cls.fields, acc, indent + 1);
        s += emitMethodList(cls.methods, acc, indent + 1, cls.name);
    }

    s += closeBrace(indent) + ";\n";
    return s;
}

// ─── Includes helper ─────────────────────────────────────────────────────────

std::string CxxEmitter::emitIncludes(
        const std::vector<std::string>& system,
        const std::vector<std::string>& user) const {
    std::string s;
    for (const auto& h : system) s += "#include <" + h + ">\n";
    for (const auto& h : user)   s += "#include \"" + h + "\"\n";
    if (!system.empty() || !user.empty()) s += "\n";
    return s;
}

// ─── Full unit emitter ────────────────────────────────────────────────────────

std::string CxxEmitter::emitUnit(const CxxUnit& unit) const {
    std::ostringstream ss;

    if (cfg_.emitFileHeader) {
        ss << "// Generated by RetDec Mixed C++/C backend\n";
        ss << "// Output: " << (unit.isCxx ? ".cpp" : ".c") << "\n\n";
    }

    ss << emitIncludes(unit.systemIncludes, unit.includes);

    for (const auto& u : unit.usings)
        ss << emitUsing(u);
    if (!unit.usings.empty()) ss << "\n";

    for (const auto& en : unit.enums)
        ss << emitEnum(en, 0) << "\n";

    for (const auto& cls : unit.classes)
        ss << emitClass(cls, 0) << "\n";

    if (!unit.globalDecls.empty())
        ss << unit.globalDecls << "\n\n";

    for (const auto& fn : unit.functions) {
        ss << emitFunction(fn, 0) << "\n";
    }

    return ss.str();
}

} // namespace cxx_backend
} // namespace retdec
