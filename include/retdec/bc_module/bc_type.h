/**
 * @file include/retdec/bc_module/bc_type.h
 * @brief BcType — unified managed-language type system.
 *
 * Covers the intersection of:
 *   - JVM descriptor types (Z B S I J F D C, reference objects, arrays)
 *   - CLR metadata types (System.Int32, System.Object, generics, constraints)
 *   - Python type hints (built-ins, Optional, Union, List, Dict, …)
 *   - WebAssembly value types (i32, i64, f32, f64, v128, funcref, externref)
 *   - Lua types (nil, boolean, integer, number, string, table, function, …)
 *
 * ## Design
 *
 * `BcType` is a value-semantic discriminated union (std::variant).  All
 * comparisons are structural — two types are equal iff their complete
 * recursive structure is equal.
 *
 * Interning is the caller's responsibility (BcModule maintains a string
 * pool so class names are shared without heap duplication).
 */

#ifndef RETDEC_BC_MODULE_BC_TYPE_H
#define RETDEC_BC_MODULE_BC_TYPE_H

#include <cstdint>
#include <memory>
#include <string>
#include <variant>
#include <vector>

namespace retdec {
namespace bc_module {

// ─── Primitive types ──────────────────────────────────────────────────────────

/**
 * Primitive type tags.  Covers the union of all managed-language primitives.
 *
 * Mapping:
 *   JVM:   void Z B S I J F D C   (+ Wasm extensions)
 *   CLR:   void bool sbyte byte short ushort int uint long ulong float double char
 *   Wasm:  i32 i64 f32 f64 v128 funcref externref
 *   Lua:   nil boolean integer number
 *   Python: uses None/bool/int/float/bytes/str (mapped to closest above)
 */
enum class BcPrimKind : uint8_t {
    Void,
    Bool,
    Byte,        ///< signed 8-bit (Java byte, C# sbyte, CLR Int8)
    UByte,       ///< unsigned 8-bit (C# byte, CLR Uint8)
    Short,       ///< signed 16-bit
    UShort,      ///< unsigned 16-bit (C# ushort, Java char mapped here)
    Char,        ///< Unicode code unit (Java char, C# char — 16-bit UTF-16)
    Int,         ///< signed 32-bit (Java int, C# int, Wasm i32)
    UInt,        ///< unsigned 32-bit (C# uint)
    Long,        ///< signed 64-bit (Java long, C# long, Wasm i64)
    ULong,       ///< unsigned 64-bit (C# ulong)
    Float,       ///< IEEE 754 32-bit (Wasm f32)
    Double,      ///< IEEE 754 64-bit (Wasm f64)
    V128,        ///< WebAssembly SIMD 128-bit vector
    FuncRef,     ///< Wasm funcref
    ExternRef,   ///< Wasm externref
    NilType,     ///< Lua nil, Python None
    LuaNumber,   ///< Lua number (float64)
    LuaInteger,  ///< Lua integer (int64 in Lua 5.3+)
    LuaString,   ///< Lua string (byte array)
};

struct BcPrimType {
    BcPrimKind kind = BcPrimKind::Void;

    bool operator==(const BcPrimType& o) const noexcept { return kind == o.kind; }
    bool operator!=(const BcPrimType& o) const noexcept { return !(*this == o); }
    std::string toString() const;
    std::string jvmDescriptor() const;  ///< "I", "Z", "V", etc.
};

// ─── Reference types ──────────────────────────────────────────────────────────

struct BcType;  // Forward declaration for recursive types.

/**
 * Reference-type kind tags.
 */
enum class BcRefKind : uint8_t {
    Class,          ///< Named class / interface reference  (java.lang.String)
    Array,          ///< Array with element type             (int[], String[][])
    Generic,        ///< Generic instantiation               (List<String>)
    Wildcard,       ///< Unbounded wildcard                  (?)
    BoundedAbove,   ///< Upper-bounded wildcard              (? extends Foo)
    BoundedBelow,   ///< Lower-bounded wildcard              (? super Foo)
    TypeVariable,   ///< Generic type parameter              (T, E, K, V)
    Null,           ///< The null type (bottom of the ref hierarchy)
};

struct BcRefType {
    BcRefKind kind = BcRefKind::Class;

    // For Class / TypeVariable:
    std::string className;        ///< Fully-qualified: "java/lang/String"

    // For Array:
    std::shared_ptr<BcType> elementType;
    int                     arrayDims = 1; ///< Number of array dimensions

    // For Generic instantiation:
    std::shared_ptr<BcType>              genericBase;   ///< Raw type
    std::vector<std::shared_ptr<BcType>> typeArgs;

    // For BoundedAbove / BoundedBelow:
    std::shared_ptr<BcType> bound;

    bool operator==(const BcRefType& o) const noexcept;
    bool operator!=(const BcRefType& o) const noexcept { return !(*this == o); }
    std::string toString() const;
    std::string jvmDescriptor() const;
};

// ─── Function / method type ───────────────────────────────────────────────────

struct BcFuncType {
    std::vector<std::shared_ptr<BcType>> params;
    std::shared_ptr<BcType>              returnType;    ///< nullptr ↔ void

    bool operator==(const BcFuncType& o) const noexcept;
    bool operator!=(const BcFuncType& o) const noexcept { return !(*this == o); }
    std::string toString() const;
    std::string jvmDescriptor() const;  ///< "(ILjava/lang/String;)V" style
};

// ─── BcType discriminated union ───────────────────────────────────────────────

using BcTypeVariant = std::variant<BcPrimType, BcRefType, BcFuncType>;

/**
 * @brief BcType — the single type representation used throughout BcModule.
 *
 * Created via the factory helpers below rather than direct construction,
 * to ensure consistency.
 */
struct BcType {
    BcTypeVariant v;

    // Discriminants
    bool isPrim()       const { return std::holds_alternative<BcPrimType>(v); }
    bool isRef()        const { return std::holds_alternative<BcRefType>(v); }
    bool isFunc()       const { return std::holds_alternative<BcFuncType>(v); }
    bool isVoid()       const;
    bool isIntegral()   const;   ///< Bool, Byte, UByte, Short, UShort, Char, Int, UInt, Long, ULong
    bool isFloating()   const;   ///< Float, Double, V128, LuaNumber
    bool isArray()      const;
    bool isClass()      const;
    bool isNullable()   const;   ///< Reference types and NilType

    const BcPrimType&  prim() const { return std::get<BcPrimType>(v); }
    const BcRefType&   ref()  const { return std::get<BcRefType>(v); }
    const BcFuncType&  func() const { return std::get<BcFuncType>(v); }

    bool operator==(const BcType& o) const noexcept;
    bool operator!=(const BcType& o) const noexcept { return !(*this == o); }

    std::string toString()       const;
    std::string jvmDescriptor()  const;
    std::string clrName()        const;  ///< "System.Int32", "System.String[]"
    std::string pythonAnnotation() const; ///< "int", "str", "List[str]"

    // Wasm value width in bytes (0 for non-wasm or reference types)
    int wasmWidth() const;
    // Stack slots consumed (JVM: long/double = 2, others = 1)
    int jvmSlots() const;
};

// ─── Type factories ───────────────────────────────────────────────────────────

namespace types {

inline BcType Void()   { return BcType{BcPrimType{BcPrimKind::Void}}; }
inline BcType Bool()   { return BcType{BcPrimType{BcPrimKind::Bool}}; }
inline BcType Byte()   { return BcType{BcPrimType{BcPrimKind::Byte}}; }
inline BcType UByte()  { return BcType{BcPrimType{BcPrimKind::UByte}}; }
inline BcType Short()  { return BcType{BcPrimType{BcPrimKind::Short}}; }
inline BcType UShort() { return BcType{BcPrimType{BcPrimKind::UShort}}; }
inline BcType Char()   { return BcType{BcPrimType{BcPrimKind::Char}}; }
inline BcType Int()    { return BcType{BcPrimType{BcPrimKind::Int}}; }
inline BcType UInt()   { return BcType{BcPrimType{BcPrimKind::UInt}}; }
inline BcType Long()   { return BcType{BcPrimType{BcPrimKind::Long}}; }
inline BcType ULong()  { return BcType{BcPrimType{BcPrimKind::ULong}}; }
inline BcType Float()  { return BcType{BcPrimType{BcPrimKind::Float}}; }
inline BcType Double() { return BcType{BcPrimType{BcPrimKind::Double}}; }
inline BcType V128()   { return BcType{BcPrimType{BcPrimKind::V128}}; }
inline BcType NilType(){ return BcType{BcPrimType{BcPrimKind::NilType}}; }

/// Named class reference ("java/lang/String", "System.String", "MyClass").
inline BcType Class(std::string name) {
    BcRefType r;
    r.kind      = BcRefKind::Class;
    r.className = std::move(name);
    return BcType{std::move(r)};
}

/// Array of a given element type and dimensionality.
inline BcType Array(BcType elem, int dims = 1) {
    BcRefType r;
    r.kind        = BcRefKind::Array;
    r.elementType = std::make_shared<BcType>(std::move(elem));
    r.arrayDims   = dims;
    return BcType{std::move(r)};
}

/// Generic type variable ("T", "E", "K").
inline BcType TypeVar(std::string name) {
    BcRefType r;
    r.kind      = BcRefKind::TypeVariable;
    r.className = std::move(name);
    return BcType{std::move(r)};
}

/// Unbounded wildcard (?).
inline BcType Wildcard() {
    BcRefType r;
    r.kind = BcRefKind::Wildcard;
    return BcType{std::move(r)};
}

/// Upper-bounded wildcard (? extends T).
inline BcType BoundedAbove(BcType bound) {
    BcRefType r;
    r.kind  = BcRefKind::BoundedAbove;
    r.bound = std::make_shared<BcType>(std::move(bound));
    return BcType{std::move(r)};
}

/// Lower-bounded wildcard (? super T).
inline BcType BoundedBelow(BcType bound) {
    BcRefType r;
    r.kind  = BcRefKind::BoundedBelow;
    r.bound = std::make_shared<BcType>(std::move(bound));
    return BcType{std::move(r)};
}

/// Generic instantiation: base<arg1, arg2, …>.
BcType Generic(BcType base, std::vector<BcType> args);

/// Function / method type (params → return).
BcType Func(std::vector<BcType> params, BcType ret);

/// Null type (bottom of the reference hierarchy).
inline BcType Null() {
    BcRefType r;
    r.kind = BcRefKind::Null;
    return BcType{std::move(r)};
}

// Convenience wrappers for frequently used class types.
inline BcType String()    { return Class("java/lang/String"); }
inline BcType Object()    { return Class("java/lang/Object"); }
inline BcType Throwable() { return Class("java/lang/Throwable"); }
inline BcType ClrString() { return Class("System.String"); }
inline BcType ClrObject() { return Class("System.Object"); }

} // namespace types

} // namespace bc_module
} // namespace retdec

#endif // RETDEC_BC_MODULE_BC_TYPE_H
