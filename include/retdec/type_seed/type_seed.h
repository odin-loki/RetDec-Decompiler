/**
 * @file include/retdec/type_seed/type_seed.h
 * @brief Type-seeding from mangled symbol names.
 *
 * ## Purpose
 *
 * Mangled C++ symbols are the richest source of ground-truth type information
 * in a stripped binary.  A single MSVC-mangled name encodes:
 *   - Full qualified name (class, namespace, function)
 *   - Calling convention (__thiscall, __fastcall, __cdecl, __stdcall, __vectorcall)
 *   - Return type (exact C++ type)
 *   - Every parameter type (with cv-qualifiers and reference category)
 *   - Class membership (implies `this` pointer as first implicit parameter)
 *
 * An Itanium-mangled name encodes:
 *   - Full nested-name (namespace::class::method<template-args>)
 *   - All parameter types with qualifiers
 *   - Return type for template functions
 *   - Template arguments for instantiations (std::vector<int> → T = int)
 *   - Whether the function is `noreturn` (via `_Z...r` modifier)
 *   - `const` member (`_ZNK...`) → `this` is `const ClassName*`
 *   - Destructor (`D0`/`D1`/`D2` encodings)
 *
 * Rust/Swift manglings add:
 *   - Trait-impl methods (impl Trait for Type)
 *   - Generic monomorphisations (fn foo::<i32, String>)
 *   - `async fn` → implicit Future return type
 *
 * ## Architecture
 *
 * Each compiler family has a concrete `ITypeSeeder` that:
 *   1. Tests whether a symbol is mangled by that ABI.
 *   2. Demangles it and parses the structural grammar.
 *   3. Emits zero or more `TypeConstraint` records.
 *
 * `TypeSeedDispatcher` owns all seeders and routes symbols to the right one.
 * The caller feeds the resulting `TypeConstraint` stream into
 * `TypeInferenceMgr::addGroundTruthConstraint(constraint)`.
 *
 * ## Constraint confidence
 *
 * All constraints extracted from mangled names carry `confidence = 1.0`.
 * A calling convention derived from mangling overrides any heuristic CC
 * detection result from the compiler fingerprinter.
 */

#ifndef RETDEC_TYPE_SEED_TYPE_SEED_H
#define RETDEC_TYPE_SEED_TYPE_SEED_H

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace retdec {
namespace type_seed {

// ─── Type representation ──────────────────────────────────────────────────────

/**
 * Compact string representation of a C/C++ type as it would appear in a
 * declaration (e.g. "int", "const char*", "std::vector<int>&",
 * "unsigned long long", "void(*)(int, float)").
 *
 * We deliberately avoid inventing a separate type-node hierarchy here —
 * the strings feed directly into the type-inference engine which has its
 * own internal representation.
 */
using TypeStr = std::string;

/** Calling convention extracted from a mangled name. */
enum class MangledCC : uint8_t {
    Unknown,
    Cdecl,
    Stdcall,
    Fastcall,   ///< MSVC __fastcall (ECX, EDX) or Borland register
    Thiscall,   ///< MSVC __thiscall (ECX for this)
    Vectorcall, ///< MSVC __vectorcall
    Clrcall,    ///< MSVC __clrcall (.NET)
    Pascal,     ///< __pascal
    Watcall,    ///< Watcom register convention
    Regparm,    ///< GCC __attribute__((regparm(N)))
    SysVAmd64,  ///< SystemV AMD64 (implicit for Linux x86-64)
    Win64,      ///< Windows x64 ABI
    AArch64,    ///< AAPCS64
};

const char* mangledCCName(MangledCC cc) noexcept;

/**
 * Reference category of a type (for C++ rvalue-ref awareness).
 */
enum class RefCategory : uint8_t { None, LValueRef, RValueRef };

/**
 * One extracted parameter type.
 */
struct ParamInfo {
    TypeStr      type;
    bool         isConst   = false;
    bool         isVolatile= false;
    RefCategory  ref       = RefCategory::None;
    std::string  name;     ///< From demangled name if available
};

/**
 * All structural information parsed from one mangled symbol name.
 */
struct SignatureInfo {
    // ── Name ──────────────────────────────────────────────────────────────────
    std::string  mangledName;
    std::string  demangledName;   ///< Full human-readable form
    std::string  functionName;    ///< Simple function/method name
    std::string  className;       ///< Enclosing class (empty if free function)
    std::string  namespaceName;   ///< Enclosing namespace (may be nested: A::B)

    // ── Types ─────────────────────────────────────────────────────────────────
    TypeStr             returnType;
    std::vector<ParamInfo> params;   ///< Explicit parameters (not `this`)
    bool                hasThis    = false; ///< True for non-static member functions
    TypeStr             thisType;           ///< e.g. "Foo*" or "const Foo*"
    bool                isConst    = false; ///< const member function (KNR `K`)
    bool                isNoReturn = false; ///< [[noreturn]] / _Noreturn
    bool                isDestructor = false;
    bool                isConstructor= false;
    bool                isOperator   = false;

    // ── Calling convention ────────────────────────────────────────────────────
    MangledCC    callingConvention = MangledCC::Unknown;

    // ── Template arguments ────────────────────────────────────────────────────
    /// Extracted template type arguments (in order), e.g. for
    /// std::vector<int, std::allocator<int>> this is {"int",
    /// "std::allocator<int>"}.
    std::vector<TypeStr> templateArgs;

    // ── Rust/Swift extras ─────────────────────────────────────────────────────
    std::string  traitName;   ///< Rust: trait being implemented
    bool         isAsync  = false; ///< Rust/Swift async fn
    bool         isUnsafe = false; ///< Rust unsafe fn

    bool valid() const noexcept { return !mangledName.empty() && !demangledName.empty(); }
};

// ─── Type constraint record ───────────────────────────────────────────────────

/**
 * Kind of type constraint being asserted.
 */
enum class ConstraintKind : uint8_t {
    ReturnType,         ///< Function at VMA returns this type
    ParamType,          ///< Parameter N of function at VMA has this type
    ThisType,           ///< Implicit this pointer type
    CallingConvention,  ///< CC override for function at VMA
    TemplateArgType,    ///< Template argument N resolved to this type
    GlobalVarType,      ///< Global variable at VMA has this type
    FieldType,          ///< Field at byte-offset within class has this type
};

/**
 * A single ground-truth type constraint derived from a mangled symbol.
 * Feed this into TypeInferenceMgr::addGroundTruthConstraint().
 */
struct TypeConstraint {
    ConstraintKind   kind        = ConstraintKind::ReturnType;
    uint64_t         symbolVma   = 0;   ///< VMA of the function / variable
    uint32_t         paramIndex  = 0;   ///< For ParamType/TemplateArgType
    TypeStr          typeStr;           ///< The asserted type string
    MangledCC        cc          = MangledCC::Unknown; ///< For CallingConvention
    float            confidence  = 1.0f;
    std::string      sourceSymbol;      ///< Original mangled name (for debug)
};

// ─── TypeInferenceMgr stub interface ─────────────────────────────────────────

/**
 * Minimal interface for the type inference engine.
 * The full implementation lives in the type inference stage.
 * This interface allows type_seed to call into it without a hard dependency.
 */
class ITypeInferenceMgr {
public:
    virtual ~ITypeInferenceMgr() = default;

    virtual void addGroundTruthConstraint(const TypeConstraint& c) = 0;

    /// Convenience: add all constraints for a fully-parsed signature.
    void addSignature(const SignatureInfo& sig, uint64_t vma);
};

// ─── Type seeder interface ────────────────────────────────────────────────────

/**
 * Abstract interface for one compiler ABI's type seeder.
 */
class ITypeSeeder {
public:
    virtual ~ITypeSeeder() = default;

    /// Return true if `symbol` is mangled by this ABI.
    virtual bool accepts(const std::string& symbol) const noexcept = 0;

    /// Demangle and extract full signature info.
    /// Returns a SignatureInfo with valid()==false on failure.
    virtual SignatureInfo extract(const std::string& symbol) const = 0;

    /// Compiler family name for diagnostics.
    virtual const char* name() const noexcept = 0;
};

// ─── Dispatcher ───────────────────────────────────────────────────────────────

/**
 * Routes symbols to the correct seeder, aggregates constraints, and feeds
 * them into the TypeInferenceMgr.
 */
class TypeSeedDispatcher {
public:
    TypeSeedDispatcher();

    /// Register a seeder (takes ownership).
    void registerSeeder(std::unique_ptr<ITypeSeeder> seeder);

    /**
     * Process one symbol at the given VMA.
     *
     * Finds the first seeder that accepts the symbol, extracts the full
     * SignatureInfo, converts it to TypeConstraint records, and calls
     * mgr.addGroundTruthConstraint() for each.
     *
     * Returns the extracted SignatureInfo (valid()==false if no seeder matched
     * or extraction failed).
     */
    SignatureInfo process(const std::string& symbol,
                          uint64_t            vma,
                          ITypeInferenceMgr&  mgr) const;

    /**
     * Process a batch of (symbol, vma) pairs.
     * Returns the number of successfully processed symbols.
     */
    uint32_t processBatch(
        const std::vector<std::pair<std::string, uint64_t>>& symbols,
        ITypeInferenceMgr& mgr) const;

    /**
     * Extract SignatureInfo without feeding into a type inference manager.
     * Useful for testing and diagnostics.
     */
    SignatureInfo tryExtract(const std::string& symbol) const;

    const std::vector<std::unique_ptr<ITypeSeeder>>& seeders() const {
        return seeders_;
    }

    static std::vector<TypeConstraint> toConstraints(
        const SignatureInfo& sig, uint64_t vma);

private:
    std::vector<std::unique_ptr<ITypeSeeder>> seeders_;
};

/// Build a TypeSeedDispatcher pre-populated with all four seeders
/// (Itanium, MSVC, Rust, Swift).
TypeSeedDispatcher makeDefaultDispatcher();

} // namespace type_seed
} // namespace retdec

#endif // RETDEC_TYPE_SEED_TYPE_SEED_H
