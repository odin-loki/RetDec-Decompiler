/**
 * @file src/csharp_emitter/cs_type_emitter.cpp
 * @brief BcClass → C# type declaration.
 */

#include "retdec/csharp_emitter/cs_type_emitter.h"

#include <algorithm>
#include <cassert>

namespace retdec {
namespace csharp_emitter {

// ─── CsTypeEmitter ───────────────────────────────────────────────────────────

CsTypeEmitter::CsTypeEmitter(CsWriter& writer,
                               const CsExprEmitter& expr,
                               const CsStmtEmitter& stmt,
                               Options opts)
    : writer_(writer), expr_(expr), stmt_(stmt), opts_(std::move(opts)) {}

// ─── Helpers ──────────────────────────────────────────────────────────────────

std::string CsTypeEmitter::accessModifiers(BcAccess access) const {
    using A = BcAccess;
    auto has = [&](A f) { return (static_cast<uint32_t>(access) & static_cast<uint32_t>(f)) != 0; };

    std::string mods;
    if (has(A::Public))    mods += "public ";
    if (has(A::Private))   mods += "private ";
    if (has(A::Protected)) mods += "protected ";
    if (has(A::Internal))  mods += "internal ";
    if (has(A::Static))    mods += "static ";
    if (has(A::Abstract))  mods += "abstract ";
    if (has(A::Sealed))    mods += "sealed ";
    if (has(A::Virtual))   mods += "virtual ";
    if (has(A::Override))  mods += "override ";
    if (has(A::Extern))    mods += "extern ";
    if (has(A::Readonly))  mods += "readonly ";

    // Remove trailing space
    if (!mods.empty() && mods.back() == ' ')
        mods.pop_back();
    return mods;
}

std::string CsTypeEmitter::methodKey(const BcClass& cls, const BcMethod& m) const {
    return cls.fqName + "::" + m.name;
}

bool CsTypeEmitter::isDelegate(const BcClass& cls) const {
    if (cls.methods.size() == 1 && cls.methods[0].name == "Invoke")
        return true;
    // Also check superclass = System.MulticastDelegate
    if (cls.superClass.has_value()) {
        std::string sc = cls.superClass->toString();
        if (sc.find("Delegate") != std::string::npos) return true;
    }
    return false;
}

bool CsTypeEmitter::isCompilerGenerated(const BcClass& cls) const {
    if (!opts_.omitCompilerGenerated) return false;
    // Compiler-generated names contain < > or start with <>
    if (cls.name.find('<') != std::string::npos) return true;
    for (const auto& ann : cls.annotations) {
        if (ann.typeName.find("CompilerGenerated") != std::string::npos) return true;
    }
    return false;
}

bool CsTypeEmitter::isPropertyAccessor(const BcMethod& m) const {
    return m.name.size() > 4 &&
           (m.name.substr(0, 4) == "get_" || m.name.substr(0, 4) == "set_");
}

bool CsTypeEmitter::isEventAccessor(const BcMethod& m) const {
    return m.name.size() > 4 &&
           (m.name.substr(0, 4) == "add_" || m.name.size() > 7 &&
            m.name.substr(0, 7) == "remove_");
}

bool CsTypeEmitter::isOperator(const BcMethod& m) const {
    return m.name.size() > 3 && m.name.substr(0, 3) == "op_";
}

// ─── Property grouping ────────────────────────────────────────────────────────

std::vector<CsTypeEmitter::PropertyGroup> CsTypeEmitter::collectProperties(
        const BcClass& cls) const {
    std::vector<PropertyGroup> groups;
    std::unordered_map<std::string, size_t> nameToIdx;

    for (const auto& m : cls.methods) {
        if (!isPropertyAccessor(m)) continue;
        bool isGetter = m.name.substr(0, 4) == "get_";
        std::string propName = m.name.substr(4);

        auto it = nameToIdx.find(propName);
        size_t idx;
        if (it == nameToIdx.end()) {
            idx = groups.size();
            nameToIdx[propName] = idx;
            PropertyGroup g;
            g.name = propName;
            g.modifiers = accessModifiers(m.access);
            bool isStatic = (static_cast<uint32_t>(m.access) &
                             static_cast<uint32_t>(BcAccess::Static)) != 0;
            g.isStatic = isStatic;
            groups.push_back(std::move(g));
        } else {
            idx = it->second;
        }

        BcMethod* mptr = const_cast<BcMethod*>(&m);
        if (isGetter) {
            groups[idx].getter = mptr;
            // Return type from getter
            if (m.descriptor.returnType)
                groups[idx].type = *m.descriptor.returnType;
        } else {
            groups[idx].setter = mptr;
        }
    }
    return groups;
}

std::vector<CsTypeEmitter::EventGroup> CsTypeEmitter::collectEvents(
        const BcClass& cls) const {
    std::vector<EventGroup> groups;
    std::unordered_map<std::string, size_t> nameToIdx;

    for (const auto& m : cls.methods) {
        if (!isEventAccessor(m)) continue;
        bool isAdder = m.name.substr(0, 4) == "add_";
        std::string evName = isAdder ? m.name.substr(4) : m.name.substr(7);

        auto it = nameToIdx.find(evName);
        size_t idx;
        if (it == nameToIdx.end()) {
            idx = groups.size();
            nameToIdx[evName] = idx;
            EventGroup g;
            g.name = evName;
            bool isStatic = (static_cast<uint32_t>(m.access) &
                             static_cast<uint32_t>(BcAccess::Static)) != 0;
            g.isStatic = isStatic;
            groups.push_back(std::move(g));
        } else {
            idx = it->second;
        }

        BcMethod* mptr = const_cast<BcMethod*>(&m);
        if (isAdder) groups[idx].adder   = mptr;
        else         groups[idx].remover = mptr;
    }
    return groups;
}

// ─── Type header ─────────────────────────────────────────────────────────────

void CsTypeEmitter::emitTypeModifiers(const BcClass& cls) {
    using A = BcAccess;
    auto has = [&](A f) { return (static_cast<uint32_t>(cls.access) & static_cast<uint32_t>(f)) != 0; };

    if (has(A::Public))   writer_.write("public ");
    if (has(A::Internal)) writer_.write("internal ");
    if (has(A::Private))  writer_.write("private ");
    // No protected for top-level types

    if (!cls.isInterface && !cls.isEnum) {
        if (has(A::Static))   writer_.write("static ");
        if (has(A::Abstract) && !cls.isInterface) writer_.write("abstract ");
        if (has(A::Sealed))   writer_.write("sealed ");
    }

    // Partial for state machines
    if (opts_.emitPartialForSMs &&
        (cls.name.find("d__") != std::string::npos)) {
        writer_.write("partial ");
    }
}

void CsTypeEmitter::emitTypeKeyword(const BcClass& cls) {
    if (cls.isEnum) {
        writer_.write("enum ");
    } else if (cls.isInterface) {
        writer_.write("interface ");
    } else if (isDelegate(cls)) {
        // Handled separately
        writer_.write("delegate ");
    } else if (cls.isRecord) {
        // C# 9+ record
        using A = BcAccess;
        bool isValue = (static_cast<uint32_t>(cls.access) & static_cast<uint32_t>(A::Sealed)) != 0 &&
                       !cls.isAbstract;
        if (isValue) writer_.write("record struct ");
        else         writer_.write("record ");
    } else {
        writer_.write("class ");
    }
}

void CsTypeEmitter::emitGenericParams(const std::vector<std::string>& typeParams) {
    if (typeParams.empty()) return;
    writer_.write("<");
    for (size_t i = 0; i < typeParams.size(); ++i) {
        if (i > 0) writer_.write(", ");
        writer_.write(typeParams[i]);
    }
    writer_.write(">");
}

void CsTypeEmitter::emitBaseList(const BcClass& cls) {
    bool first = true;
    auto sep = [&]() { return first ? (first = false, " : ") : ", "; };

    if (cls.superClass.has_value()) {
        std::string sc = cls.superClass->toString();
        if (sc != "System.Object" && sc != "object" &&
            sc != "System.ValueType" && sc != "System.Enum" &&
            sc != "System.MulticastDelegate") {
            writer_.write(sep());
            writer_.write(CsWriter::clrToCsharpType(sc));
        }
    }
    for (const auto& iface : cls.interfaces) {
        writer_.write(sep());
        writer_.write(CsWriter::clrToCsharpType(iface.toString()));
    }
}

void CsTypeEmitter::emitGenericConstraints(const std::vector<std::string>& typeParams) {
    for (const auto& tp : typeParams) {
        // TODO: extract constraint info from generic param annotations
        // For now, skip
        (void)tp;
    }
}

void CsTypeEmitter::emitTypeHeader(const BcClass& cls) {
    if (isDelegate(cls) && !cls.methods.empty()) {
        // delegate return-type TypeName(params);
        const BcMethod& invoke = cls.methods[0];
        emitTypeModifiers(cls);
        if (invoke.descriptor.returnType)
            writer_.write(expr_.emitType(*invoke.descriptor.returnType) + " ");
        else
            writer_.write("void ");
        writer_.write("delegate ");
        writer_.write(CsWriter::safeName(cls.name));
        emitGenericParams(cls.typeParams);
        // Parameters
        writer_.write("(");
        for (size_t i = 0; i < invoke.descriptor.params.size(); ++i) {
            if (i > 0) writer_.write(", ");
            const auto& pt = *invoke.descriptor.params[i];
            writer_.write(expr_.emitType(pt));
            writer_.write(" ");
            std::string pname = (i < invoke.paramNames.size()) ? invoke.paramNames[i]
                                                               : "arg" + std::to_string(i);
            writer_.write(CsWriter::safeName(pname));
        }
        writer_.write(")");
        emitGenericConstraints(cls.typeParams);
        writer_.nl();
        writer_.line(";");
        return;
    }

    emitTypeModifiers(cls);
    emitTypeKeyword(cls);
    writer_.write(CsWriter::safeName(cls.name));
    emitGenericParams(cls.typeParams);
    emitBaseList(cls);
    emitGenericConstraints(cls.typeParams);
    writer_.nl();
}

// ─── Fields ───────────────────────────────────────────────────────────────────

void CsTypeEmitter::emitField(const BcField& f) {
    using A = BcAccess;
    auto has = [&](A fl) { return (static_cast<uint32_t>(f.access) & static_cast<uint32_t>(fl)) != 0; };

    // Modifiers
    std::string mods = accessModifiers(f.access);
    if (!mods.empty()) mods += " ";

    // const?
    bool isConst = has(A::Static) && has(A::Final) &&
                   (f.constantIntValue.has_value() || f.constantFltValue.has_value() ||
                    f.constantStrValue.has_value());
    std::string typeName = expr_.emitType(f.type);

    if (isConst) {
        // Emit as const
        std::string val;
        if (f.constantIntValue) val = std::to_string(*f.constantIntValue);
        else if (f.constantFltValue) val = std::to_string(*f.constantFltValue);
        else if (f.constantStrValue) val = writer_.stringLiteral(*f.constantStrValue);
        // Remove static from const
        std::string constMods = mods;
        size_t sp = constMods.find("static ");
        if (sp != std::string::npos) constMods.erase(sp, 7);
        writer_.line(constMods + "const " + typeName + " " + CsWriter::safeName(f.name) + " = " + val + ";");
        return;
    }

    writer_.line(mods + typeName + " " + CsWriter::safeName(f.name) + ";");
}

void CsTypeEmitter::emitFields(const BcClass& cls) {
    using A = BcAccess;
    auto isStatic = [](const BcField& f) {
        return (static_cast<uint32_t>(f.access) & static_cast<uint32_t>(A::Static)) != 0;
    };

    // Static fields first, then instance
    bool hasSep = false;
    for (const auto& f : cls.fields) {
        if (isStatic(f)) { emitField(f); hasSep = true; }
    }
    for (const auto& f : cls.fields) {
        if (!isStatic(f)) { emitField(f); hasSep = true; }
    }
    if (hasSep) writer_.blank();
}

// ─── Enum ────────────────────────────────────────────────────────────────────

void CsTypeEmitter::emitEnum(const BcClass& cls) {
    emitTypeHeader(cls);
    {
        auto g = writer_.block();
        for (size_t i = 0; i < cls.enumConstants.size(); ++i) {
            std::string comma = (i + 1 < cls.enumConstants.size()) ? "," : "";
            // Try to find field with this name to get its value
            const BcField* fld = cls.findField(cls.enumConstants[i]);
            if (fld && fld->constantIntValue.has_value()) {
                writer_.line(CsWriter::safeName(cls.enumConstants[i]) + " = " +
                             std::to_string(*fld->constantIntValue) + comma);
            } else {
                writer_.line(CsWriter::safeName(cls.enumConstants[i]) + comma);
            }
        }
    }
}

// ─── Properties ──────────────────────────────────────────────────────────────

void CsTypeEmitter::emitAutoProperty(const std::string& propName, const BcType& propType,
                                       bool hasGetter, bool hasSetter, bool isStatic,
                                       const std::string& modifiers) {
    std::string typeName = expr_.emitType(propType);
    std::string staticMod = isStatic ? "static " : "";
    std::string accessors;
    if (hasGetter) accessors += "get; ";
    if (hasSetter) accessors += "set; ";
    writer_.line(modifiers + " " + staticMod + typeName + " " +
                 CsWriter::safeName(propName) + " { " + accessors + "}");
}

void CsTypeEmitter::emitComputedProperty(
        const std::string& propName, const BcType& propType,
        const BcMethod* getter, const BcMethod* setter,
        const std::unordered_map<std::string, CilReconstructResult>& results,
        const std::string& modifiers) {
    std::string typeName = expr_.emitType(propType);
    bool isStatic = getter &&
        (static_cast<uint32_t>(getter->access) & static_cast<uint32_t>(BcAccess::Static)) != 0;
    std::string staticMod = isStatic ? "static " : "";

    writer_.line(modifiers + " " + staticMod + typeName + " " + CsWriter::safeName(propName));
    {
        auto g = writer_.block();

        if (getter) {
            writer_.line("get");
            auto g2 = writer_.block();
            // emit getter body
            if (!getter->name.empty()) {
                auto it = results.find(std::string(getter->name));
                if (it != results.end() && !it->second.method.body.empty()) {
                    const_cast<CsStmtEmitter&>(stmt_).emitBody(it->second.method.body);
                } else {
                    writer_.line("throw new NotImplementedException();");
                }
            }
        }

        if (setter) {
            writer_.line("set");
            auto g2 = writer_.block();
            if (!setter->name.empty()) {
                auto it = results.find(std::string(setter->name));
                if (it != results.end() && !it->second.method.body.empty()) {
                    const_cast<CsStmtEmitter&>(stmt_).emitBody(it->second.method.body);
                } else {
                    writer_.line("throw new NotImplementedException();");
                }
            }
        }
    }
}

void CsTypeEmitter::emitProperties(
        const BcClass& cls,
        const std::unordered_map<std::string, CilReconstructResult>& results) {
    auto props = collectProperties(cls);
    if (props.empty()) return;

    for (const auto& pg : props) {
        bool isAutoProperty = opts_.preferAutoProperties &&
                              pg.getter && pg.setter &&
                              pg.getter->cfg.blockCount() <= 1 &&
                              pg.setter->cfg.blockCount() <= 1;

        if (isAutoProperty) {
            emitAutoProperty(pg.name, pg.type, !!pg.getter, !!pg.setter,
                             pg.isStatic, pg.modifiers);
        } else {
            emitComputedProperty(pg.name, pg.type, pg.getter, pg.setter,
                                 results, pg.modifiers);
        }
    }
    writer_.blank();
}

// ─── Events ───────────────────────────────────────────────────────────────────

void CsTypeEmitter::emitEvents(
        const BcClass& cls,
        const std::unordered_map<std::string, CilReconstructResult>&) {
    auto evs = collectEvents(cls);
    for (const auto& eg : evs) {
        std::string typeName = expr_.emitType(eg.type);
        std::string staticMod = eg.isStatic ? "static " : "";
        writer_.line("public " + staticMod + "event " + typeName + " " +
                     CsWriter::safeName(eg.name) + ";");
    }
    if (!evs.empty()) writer_.blank();
}

// ─── Methods ─────────────────────────────────────────────────────────────────

void CsTypeEmitter::emitMethodSignature(const BcClass& cls, const BcMethod& m) {
    using A = BcAccess;

    std::string mods = accessModifiers(m.access);

    // Determine if this is a named operator overload
    bool wrotePrefix = false;
    if (isOperator(m)) {
        static const std::unordered_map<std::string,std::string> opNames = {
            {"op_Addition","+"},{"op_Subtraction","-"},{"op_Multiply","*"},
            {"op_Division","/"},{"op_Modulus","%"},{"op_BitwiseAnd","&"},
            {"op_BitwiseOr","|"},{"op_ExclusiveOr","^"},{"op_LeftShift","<<"},
            {"op_RightShift",">>"},{"op_Equality","=="},{"op_Inequality","!="},
            {"op_LessThan","<"},{"op_GreaterThan",">"},{"op_LessThanOrEqual","<="},
            {"op_GreaterThanOrEqual",">="},{"op_UnaryNegation","-"},
            {"op_UnaryPlus","+"},{"op_LogicalNot","!"},{"op_OnesComplement","~"},
            {"op_Increment","++"},{"op_Decrement","--"},
            {"op_Implicit",""},{"op_Explicit",""},
        };
        auto it = opNames.find(m.name);
        if (it != opNames.end()) {
            std::string retType = m.descriptor.returnType ?
                expr_.emitType(*m.descriptor.returnType) : "void";
            if (m.name == "op_Implicit")
                writer_.write(mods + " implicit operator " + retType);
            else if (m.name == "op_Explicit")
                writer_.write(mods + " explicit operator " + retType);
            else
                writer_.write(mods + " static " + retType + " operator " + it->second);
            wrotePrefix = true;
        }
    }

    if (!wrotePrefix) {
        if (m.isConstructor) {
            writer_.write(mods + " ");
            writer_.write(CsWriter::safeName(cls.name));
        } else {
            std::string retType = m.descriptor.returnType ?
                expr_.emitType(*m.descriptor.returnType) : "void";

            bool isAsync = retType.find("Task") != std::string::npos;

            writer_.write(mods + " ");
            if (isAsync) writer_.write("async ");
            writer_.write(retType + " ");
            writer_.write(CsWriter::safeName(m.name));
        }

        // Generic params
        if (!m.typeParams.empty()) {
            writer_.write("<");
            for (size_t i = 0; i < m.typeParams.size(); ++i) {
                if (i > 0) writer_.write(", ");
                writer_.write(m.typeParams[i]);
            }
            writer_.write(">");
        }
    }

    // Parameters
    writer_.write("(");
    for (size_t i = 0; i < m.descriptor.params.size(); ++i) {
        if (i > 0) writer_.write(", ");
        const auto& pt = *m.descriptor.params[i];
        writer_.write(expr_.emitType(pt));
        writer_.write(" ");
        std::string pname = (i < m.paramNames.size()) ? m.paramNames[i]
                                                      : "arg" + std::to_string(i);
        writer_.write(CsWriter::safeName(pname));
    }
    writer_.write(")");
}

void CsTypeEmitter::emitMethodBody(
        const BcMethod& m,
        const std::unordered_map<std::string, CilReconstructResult>& results) {
    auto it = results.find(m.name);

    if (m.isAbstract || m.isNative) {
        writer_.line(";");
        return;
    }

    writer_.nl();

    if (it != results.end() && !it->second.method.body.empty()) {
        auto g = writer_.block();
        const_cast<CsStmtEmitter&>(stmt_).emitBody(it->second.method.body);
    } else if (m.cfg.blockCount() == 0) {
        // Abstract / extern / native
        writer_.line("{");
        writer_.line("    throw new NotImplementedException();");
        writer_.line("}");
    } else {
        auto g = writer_.block();
        writer_.line("throw new NotImplementedException();");
    }
}

void CsTypeEmitter::emitConstructors(
        const BcClass& cls,
        const std::unordered_map<std::string, CilReconstructResult>& results) {
    for (const auto& m : cls.methods) {
        if (!m.isConstructor) continue;
        emitMethod(cls, m, results);
    }
}

void CsTypeEmitter::emitDestructor(
        const BcClass& cls,
        const std::unordered_map<std::string, CilReconstructResult>& results) {
    const BcMethod* fin = cls.findMethod("Finalize");
    if (!fin) return;
    writer_.write("~");
    writer_.write(CsWriter::safeName(cls.name));
    writer_.write("()");
    emitMethodBody(*fin, results);
}

void CsTypeEmitter::emitMethod(
        const BcClass& cls, const BcMethod& m,
        const std::unordered_map<std::string, CilReconstructResult>& results) {
    // Skip compiler-generated, property/event accessors, finalizer (handled separately)
    if (isPropertyAccessor(m) || isEventAccessor(m)) return;
    if (m.name == "Finalize") return;
    if (m.isStaticInit) {
        // Static constructor
        writer_.write("static ");
        writer_.write(CsWriter::safeName(cls.name));
        writer_.write("()");
        emitMethodBody(m, results);
        writer_.blank();
        return;
    }

    emitMethodSignature(cls, m);
    emitMethodBody(m, results);
    writer_.blank();
}

void CsTypeEmitter::emitMethods(
        const BcClass& cls,
        const std::unordered_map<std::string, CilReconstructResult>& results) {
    for (const auto& m : cls.methods) {
        if (isPropertyAccessor(m) || isEventAccessor(m)) continue;
        if (m.isConstructor) continue; // handled in emitConstructors
        if (m.name == "Finalize") continue;
        emitMethod(cls, m, results);
    }
}

// ─── emitClass ────────────────────────────────────────────────────────────────

void CsTypeEmitter::emitClass(
        const BcClass& cls,
        const std::unordered_map<std::string, CilReconstructResult>& results,
        const BcModule& module) {
    (void)module;

    if (cls.isEnum) {
        emitEnum(cls);
        return;
    }

    if (isDelegate(cls)) {
        emitTypeHeader(cls);
        return;
    }

    emitTypeHeader(cls);
    {
        auto g = writer_.block();

        emitFields(cls);
        emitConstructors(cls, results);
        emitDestructor(cls, results);
        emitProperties(cls, results);
        emitEvents(cls, results);
        emitMethods(cls, results);
    }
}

} // namespace csharp_emitter
} // namespace retdec
