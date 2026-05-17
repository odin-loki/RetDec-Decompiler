/**
 * @file src/java_emitter/java_type_printer.cpp
 * @brief BcType → Java source type name with import tracking.
 */

#include "retdec/java_emitter/java_type_printer.h"

#include <algorithm>

namespace retdec {
namespace java_emitter {

using namespace bc_module;

// ─── ImportSet ────────────────────────────────────────────────────────────────

ImportSet::ImportSet(const std::string& currentPackage,
                     const std::string& currentClass)
    : currentPackage_(currentPackage), currentClass_(currentClass) {}

bool ImportSet::isJavaLang(const std::string& fqName) {
    // "java.lang.X" with no further dots after the prefix = java.lang.
    if (fqName.size() <= 10) return false;
    if (fqName.substr(0, 10) != "java.lang.") return false;
    return fqName.find('.', 10) == std::string::npos;
}

std::string ImportSet::simpleName(const std::string& fqName) {
    size_t pos = fqName.rfind('.');
    return (pos == std::string::npos) ? fqName : fqName.substr(pos + 1);
}

std::string ImportSet::packageOf(const std::string& fqName) {
    size_t pos = fqName.rfind('.');
    return (pos == std::string::npos) ? "" : fqName.substr(0, pos);
}

std::string ImportSet::require(const std::string& fqName) {
    if (fqName.empty() || fqName == "null") return fqName;

    // java.lang types are always available without import.
    if (isJavaLang(fqName)) return simpleName(fqName);

    // Same-package types need no import.
    if (!currentPackage_.empty() && packageOf(fqName) == currentPackage_)
        return simpleName(fqName);

    // Primitive-looking names (no dots) need no import.
    if (fqName.find('.') == std::string::npos)
        return fqName;

    std::string simple = simpleName(fqName);

    // Check for name collision.
    auto it = simpleTofq_.find(simple);
    if (it == simpleTofq_.end()) {
        simpleTofq_[simple] = fqName;
        toImport_.insert(fqName);
        return simple;
    }
    if (it->second == fqName)
        return simple; // Same type, already registered.
    // Collision: use FQ name.
    return fqName;
}

bool ImportSet::hasConflict(const std::string& simName,
                             const std::string& fqName) const {
    auto it = simpleTofq_.find(simName);
    return it != simpleTofq_.end() && it->second != fqName;
}

std::vector<std::string> ImportSet::importLines() const {
    std::vector<std::string> lines;
    lines.reserve(toImport_.size());
    for (const auto& fq : toImport_)
        lines.push_back("import " + fq + ";");
    return lines;
}

// ─── JavaTypePrinter ─────────────────────────────────────────────────────────

JavaTypePrinter::JavaTypePrinter(ImportSet& imports)
    : imports_(imports) {}

std::string JavaTypePrinter::printPrim(BcPrimType prim) const {
    switch (prim.kind) {
        case BcPrimKind::Void:      return "void";
        case BcPrimKind::Bool:      return "boolean";
        case BcPrimKind::Byte:      return "byte";
        case BcPrimKind::UByte:     return "byte";     // no unsigned in Java
        case BcPrimKind::Short:     return "short";
        case BcPrimKind::UShort:    return "short";
        case BcPrimKind::Char:      return "char";
        case BcPrimKind::Int:       return "int";
        case BcPrimKind::UInt:      return "int";      // no unsigned in Java
        case BcPrimKind::Long:      return "long";
        case BcPrimKind::ULong:     return "long";
        case BcPrimKind::Float:     return "float";
        case BcPrimKind::Double:    return "double";
        case BcPrimKind::V128:      return "long[]";   // best approximation
        case BcPrimKind::FuncRef:   return "Object";   // wasm funcref
        case BcPrimKind::ExternRef: return "Object";
        case BcPrimKind::NilType:   return "null";     // used in typeof context
        case BcPrimKind::LuaNumber: return "double";
        case BcPrimKind::LuaInteger:return "long";
        case BcPrimKind::LuaString: return "String";
        default: return "Object";
    }
}

// Convert slash-separated class name to dot-separated.
static std::string slashToDot(std::string s) {
    for (char& c : s) if (c == '/') c = '.';
    // Strip trailing ';' if present (from JVM descriptors like "Ljava/lang/String;").
    if (!s.empty() && s.back() == ';') s.pop_back();
    // Strip leading 'L' if present (from JVM descriptors).
    if (s.size() > 1 && s[0] == 'L') s = s.substr(1);
    return s;
}

std::string JavaTypePrinter::printRef(const BcRefType& ref,
                                       bool doImport) const {
    switch (ref.kind) {
        case BcRefKind::Class: {
            if (ref.className.empty()) return "Object";
            std::string fq = slashToDot(ref.className);
            if (fq == "null") return "null";
            if (doImport)
                return imports_.require(fq);
            else {
                size_t dot = fq.rfind('.');
                return (dot == std::string::npos) ? fq : fq.substr(dot + 1);
            }
        }
        case BcRefKind::Null:
            return "null";

        case BcRefKind::Array: {
            std::string elem = ref.elementType
                ? printImpl(*ref.elementType, doImport)
                : "Object";
            std::string brackets;
            for (int d = 0; d < ref.arrayDims; ++d) brackets += "[]";
            return elem + brackets;
        }
        case BcRefKind::Generic: {
            // genericBase holds the raw type; typeArgs holds the arguments.
            std::string base;
            if (ref.genericBase) {
                base = printImpl(*ref.genericBase, doImport);
            } else if (!ref.className.empty()) {
                // Fallback: className holds the base class name.
                std::string fq = slashToDot(ref.className);
                base = doImport ? imports_.require(fq)
                                : [&]() {
                    size_t dot = fq.rfind('.');
                    return dot == std::string::npos ? fq : fq.substr(dot + 1);
                }();
            } else {
                base = "Object";
            }
            if (ref.typeArgs.empty()) return base;
            std::string out = base + "<";
            for (size_t i = 0; i < ref.typeArgs.size(); ++i) {
                if (i) out += ", ";
                out += ref.typeArgs[i] ? printImpl(*ref.typeArgs[i], doImport)
                                       : "?";
            }
            out += ">";
            return out;
        }
        case BcRefKind::TypeVariable:
            return ref.className.empty() ? "?" : ref.className;

        case BcRefKind::Wildcard:
            return "?";

        case BcRefKind::BoundedAbove:
            if (!ref.bound) return "?";
            return "? extends " + printImpl(*ref.bound, doImport);

        case BcRefKind::BoundedBelow:
            if (!ref.bound) return "?";
            return "? super " + printImpl(*ref.bound, doImport);

        default:
            return "Object";
    }
}

std::string JavaTypePrinter::printImpl(const BcType& type,
                                        bool doImport) const {
    if (type.isPrim()) return printPrim(type.prim());
    if (type.isRef())  return printRef(type.ref(), doImport);
    // BcFuncType — render as a lambda-ish annotation (best-effort for Java).
    if (type.isFunc()) {
        const BcFuncType& f = type.func();
        std::string ret = f.returnType ? printImpl(*f.returnType, doImport)
                                       : "void";
        (void)ret; // For Java we just use the functional interface name.
        return "Object"; // functional interface — caller should use the interface type
    }
    return "void";
}

std::string JavaTypePrinter::print(const BcType& type) const {
    return printImpl(type, true);
}

std::string JavaTypePrinter::printNoImport(const BcType& type) const {
    return printImpl(type, false);
}

std::string JavaTypePrinter::printMethod(const BcFuncType& func,
                                          std::vector<std::string>& outParams) const {
    outParams.clear();
    for (const auto& param : func.params) {
        if (param)
            outParams.push_back(print(*param));
        else
            outParams.push_back("Object");
    }
    return func.returnType ? print(*func.returnType) : "void";
}

} // namespace java_emitter
} // namespace retdec
