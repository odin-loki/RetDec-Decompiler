/**
 * @file src/bc_module/bc_type.cpp
 * @brief BcType — equality, string rendering, descriptor helpers.
 */

#include <memory>
#include "retdec/bc_module/bc_type.h"

#include <algorithm>
#include <stdexcept>

namespace retdec {
namespace bc_module {

// ─── BcPrimType ───────────────────────────────────────────────────────────────

std::string BcPrimType::toString() const {
    switch (kind) {
    case BcPrimKind::Void:       return "void";
    case BcPrimKind::Bool:       return "boolean";
    case BcPrimKind::Byte:       return "byte";
    case BcPrimKind::UByte:      return "ubyte";
    case BcPrimKind::Short:      return "short";
    case BcPrimKind::UShort:     return "ushort";
    case BcPrimKind::Char:       return "char";
    case BcPrimKind::Int:        return "int";
    case BcPrimKind::UInt:       return "uint";
    case BcPrimKind::Long:       return "long";
    case BcPrimKind::ULong:      return "ulong";
    case BcPrimKind::Float:      return "float";
    case BcPrimKind::Double:     return "double";
    case BcPrimKind::V128:       return "v128";
    case BcPrimKind::FuncRef:    return "funcref";
    case BcPrimKind::ExternRef:  return "externref";
    case BcPrimKind::NilType:    return "nil";
    case BcPrimKind::LuaNumber:  return "number";
    case BcPrimKind::LuaInteger: return "integer";
    case BcPrimKind::LuaString:  return "string";
    }
    return "?prim";
}

std::string BcPrimType::jvmDescriptor() const {
    switch (kind) {
    case BcPrimKind::Void:   return "V";
    case BcPrimKind::Bool:   return "Z";
    case BcPrimKind::Byte:   return "B";
    case BcPrimKind::Char:   return "C";
    case BcPrimKind::Short:  return "S";
    case BcPrimKind::Int:    return "I";
    case BcPrimKind::Long:   return "J";
    case BcPrimKind::Float:  return "F";
    case BcPrimKind::Double: return "D";
    default:                 return "I";  // Best approximation for non-JVM prims
    }
}

// ─── BcRefType ────────────────────────────────────────────────────────────────

static bool typeEq(const std::shared_ptr<BcType>& a, const std::shared_ptr<BcType>& b) {
    if (!a && !b) return true;
    if (!a || !b) return false;
    return *a == *b;
}

bool BcRefType::operator==(const BcRefType& o) const noexcept {
    if (kind != o.kind) return false;
    switch (kind) {
    case BcRefKind::Class:
    case BcRefKind::TypeVariable:
        return className == o.className;
    case BcRefKind::Array:
        return arrayDims == o.arrayDims && typeEq(elementType, o.elementType);
    case BcRefKind::Generic:
        if (!typeEq(genericBase, o.genericBase)) return false;
        if (typeArgs.size() != o.typeArgs.size()) return false;
        for (size_t i = 0; i < typeArgs.size(); ++i)
            if (!typeEq(typeArgs[i], o.typeArgs[i])) return false;
        return true;
    case BcRefKind::Wildcard:
    case BcRefKind::Null:
        return true;
    case BcRefKind::BoundedAbove:
    case BcRefKind::BoundedBelow:
        return typeEq(bound, o.bound);
    }
    return false;
}

std::string BcRefType::toString() const {
    switch (kind) {
    case BcRefKind::Class:
    case BcRefKind::TypeVariable:
        return className;
    case BcRefKind::Null:
        return "null";
    case BcRefKind::Wildcard:
        return "?";
    case BcRefKind::BoundedAbove:
        return "? extends " + (bound ? bound->toString() : "?");
    case BcRefKind::BoundedBelow:
        return "? super "   + (bound ? bound->toString() : "?");
    case BcRefKind::Array: {
        std::string base = elementType ? elementType->toString() : "?";
        std::string dims(static_cast<size_t>(arrayDims) * 2, ' ');
        for (int i = 0; i < arrayDims; ++i) { dims[i*2]='['; dims[i*2+1]=']'; }
        return base + dims;
    }
    case BcRefKind::Generic: {
        std::string s = genericBase ? genericBase->toString() : "?";
        s += '<';
        for (size_t i = 0; i < typeArgs.size(); ++i) {
            if (i) s += ", ";
            s += typeArgs[i] ? typeArgs[i]->toString() : "?";
        }
        s += '>';
        return s;
    }
    }
    return "?ref";
}

std::string BcRefType::jvmDescriptor() const {
    switch (kind) {
    case BcRefKind::Class:
    case BcRefKind::TypeVariable:
        return "L" + className + ";";
    case BcRefKind::Array: {
        std::string dims(static_cast<size_t>(arrayDims), '[');
        return dims + (elementType ? elementType->jvmDescriptor() : "?");
    }
    case BcRefKind::Generic:
        return genericBase ? genericBase->jvmDescriptor() : "Ljava/lang/Object;";
    default:
        return "Ljava/lang/Object;";
    }
}

// ─── BcFuncType ───────────────────────────────────────────────────────────────

bool BcFuncType::operator==(const BcFuncType& o) const noexcept {
    if (!typeEq(returnType, o.returnType)) return false;
    if (params.size() != o.params.size())  return false;
    for (size_t i = 0; i < params.size(); ++i)
        if (!typeEq(params[i], o.params[i])) return false;
    return true;
}

std::string BcFuncType::toString() const {
    std::string s = "(";
    for (size_t i = 0; i < params.size(); ++i) {
        if (i) s += ", ";
        s += params[i] ? params[i]->toString() : "?";
    }
    s += ") -> ";
    s += returnType ? returnType->toString() : "void";
    return s;
}

std::string BcFuncType::jvmDescriptor() const {
    std::string s = "(";
    for (const auto& p : params) s += p ? p->jvmDescriptor() : "";
    s += ")";
    s += returnType ? returnType->jvmDescriptor() : "V";
    return s;
}

// ─── BcType ───────────────────────────────────────────────────────────────────

bool BcType::isVoid() const {
    return isPrim() && prim().kind == BcPrimKind::Void;
}

bool BcType::isIntegral() const {
    if (!isPrim()) return false;
    switch (prim().kind) {
    case BcPrimKind::Bool:  case BcPrimKind::Byte:   case BcPrimKind::UByte:
    case BcPrimKind::Short: case BcPrimKind::UShort: case BcPrimKind::Char:
    case BcPrimKind::Int:   case BcPrimKind::UInt:
    case BcPrimKind::Long:  case BcPrimKind::ULong:
    case BcPrimKind::LuaInteger:
        return true;
    default: return false;
    }
}

bool BcType::isFloating() const {
    if (!isPrim()) return false;
    switch (prim().kind) {
    case BcPrimKind::Float: case BcPrimKind::Double:
    case BcPrimKind::V128:  case BcPrimKind::LuaNumber:
        return true;
    default: return false;
    }
}

bool BcType::isArray() const {
    return isRef() && ref().kind == BcRefKind::Array;
}

bool BcType::isClass() const {
    return isRef() && (ref().kind == BcRefKind::Class || ref().kind == BcRefKind::Generic);
}

bool BcType::isNullable() const {
    if (isRef()) return true;
    return isPrim() && prim().kind == BcPrimKind::NilType;
}

bool BcType::operator==(const BcType& o) const noexcept {
    return v == o.v;
}

std::string BcType::toString() const {
    if (isPrim()) return prim().toString();
    if (isRef())  return ref().toString();
    if (isFunc()) return func().toString();
    return "?";
}

std::string BcType::jvmDescriptor() const {
    if (isPrim()) return prim().jvmDescriptor();
    if (isRef())  return ref().jvmDescriptor();
    if (isFunc()) return func().jvmDescriptor();
    return "";
}

std::string BcType::clrName() const {
    if (isPrim()) {
        switch (prim().kind) {
        case BcPrimKind::Void:   return "System.Void";
        case BcPrimKind::Bool:   return "System.Boolean";
        case BcPrimKind::Byte:   return "System.SByte";
        case BcPrimKind::UByte:  return "System.Byte";
        case BcPrimKind::Short:  return "System.Int16";
        case BcPrimKind::UShort: return "System.UInt16";
        case BcPrimKind::Char:   return "System.Char";
        case BcPrimKind::Int:    return "System.Int32";
        case BcPrimKind::UInt:   return "System.UInt32";
        case BcPrimKind::Long:   return "System.Int64";
        case BcPrimKind::ULong:  return "System.UInt64";
        case BcPrimKind::Float:  return "System.Single";
        case BcPrimKind::Double: return "System.Double";
        default:                 return "System.Object";
        }
    }
    if (isRef()) {
        if (ref().kind == BcRefKind::Array)
            return (ref().elementType ? ref().elementType->clrName() : "System.Object") + "[]";
        return ref().className;
    }
    return "System.Object";
}

std::string BcType::pythonAnnotation() const {
    if (isPrim()) {
        switch (prim().kind) {
        case BcPrimKind::Void:       return "None";
        case BcPrimKind::Bool:       return "bool";
        case BcPrimKind::Int:  case BcPrimKind::Long:
        case BcPrimKind::Short: case BcPrimKind::Byte:
        case BcPrimKind::UInt: case BcPrimKind::ULong:
        case BcPrimKind::LuaInteger: return "int";
        case BcPrimKind::Float:  case BcPrimKind::Double:
        case BcPrimKind::LuaNumber:  return "float";
        case BcPrimKind::Char:       return "str";
        case BcPrimKind::NilType:    return "None";
        default:                     return "object";
        }
    }
    if (isRef()) {
        if (ref().kind == BcRefKind::Array)
            return "List[" + (ref().elementType ? ref().elementType->pythonAnnotation() : "object") + "]";
        const auto& n = ref().className;
        if (n == "java/lang/String" || n == "System.String") return "str";
        if (n == "java/lang/Object" || n == "System.Object") return "object";
        // Use the simple class name.
        auto pos = n.rfind('/');
        if (pos == std::string::npos) pos = n.rfind('.');
        return (pos == std::string::npos) ? n : n.substr(pos + 1);
    }
    return "object";
}

int BcType::wasmWidth() const {
    if (!isPrim()) return 0;
    switch (prim().kind) {
    case BcPrimKind::Bool: case BcPrimKind::Int: return 4;
    case BcPrimKind::Long:  return 8;
    case BcPrimKind::Float: return 4;
    case BcPrimKind::Double:return 8;
    case BcPrimKind::V128:  return 16;
    default: return 0;
    }
}

int BcType::jvmSlots() const {
    if (!isPrim()) return 1;
    switch (prim().kind) {
    case BcPrimKind::Long:  case BcPrimKind::Double: return 2;
    default: return 1;
    }
}

// ─── Types factories ──────────────────────────────────────────────────────────

namespace types {

BcType Generic(BcType base, std::vector<BcType> args) {
    BcRefType r;
    r.kind        = BcRefKind::Generic;
    r.genericBase = std::make_shared<BcType>(std::move(base));
    for (auto& a : args)
        r.typeArgs.push_back(std::make_shared<BcType>(std::move(a)));
    return BcType{std::move(r)};
}

BcType Func(std::vector<BcType> params, BcType ret) {
    BcFuncType ft;
    for (auto& p : params)
        ft.params.push_back(std::make_shared<BcType>(std::move(p)));
    ft.returnType = std::make_shared<BcType>(std::move(ret));
    return BcType{std::move(ft)};
}

} // namespace types

} // namespace bc_module
} // namespace retdec
