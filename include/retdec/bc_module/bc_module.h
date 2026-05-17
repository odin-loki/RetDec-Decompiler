/**
 * @file include/retdec/bc_module/bc_module.h
 * @brief BcModule — the unified in-memory IR for managed-language decompilation.
 *
 * ## Hierarchy
 *
 *   BcModule
 *     └── BcClass (one per type / class / interface / enum / record)
 *           ├── BcField   (one per field)
 *           └── BcMethod  (one per method / constructor / initialiser)
 *                 ├── BcLocalVar   (named locals with live ranges)
 *                 └── BcCFG        (control-flow graph with typed stacks)
 *
 * ## Output contract
 *
 * Source emitters (Java emitter, C# emitter, Python emitter, …) consume
 * BcModule as their sole input and MUST produce:
 *   - Compilable, runnable source code in the target language.
 *   - Idiomatic constructs (for loops not while loops where the CFG pattern
 *     is recognisable, enhanced for-each for iterator patterns, etc.).
 *   - NO pseudocode.  NO IR dumps.  The output must be source code.
 *
 * ## Source language
 *
 * `BcModule::sourceLang` records the original platform so emitters can
 * make platform-specific decisions (e.g. whether to emit Java checked
 * exception signatures, or C# `using` blocks for IDisposable types).
 */

#ifndef RETDEC_BC_MODULE_BC_MODULE_H
#define RETDEC_BC_MODULE_BC_MODULE_H

#include "retdec/bc_module/bc_cfg.h"
#include "retdec/bc_module/bc_type.h"

#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace retdec {
namespace bc_module {

// ─── Source language ──────────────────────────────────────────────────────────

enum class SourceLang : uint8_t {
    Unknown,
    Java,
    CSharp,
    Python,
    WebAssembly,
    Lua,
    Kotlin,      ///< Kotlin/JVM (JVM bytecode, Kotlin metadata)
    Scala,       ///< Scala/JVM
    Groovy,
    Clojure,
    FSharp,      ///< F#/CLR
    VisualBasic, ///< VB.NET/CLR
};

std::string sourceLangName(SourceLang lang) noexcept;

// ─── Access flags ─────────────────────────────────────────────────────────────

enum class BcAccess : uint32_t {
    None        = 0,
    Public      = 1 << 0,
    Private     = 1 << 1,
    Protected   = 1 << 2,
    Internal    = 1 << 3,   ///< CLR internal / package-private (Java)
    Static      = 1 << 4,
    Final       = 1 << 5,
    Abstract    = 1 << 6,
    Synchronized= 1 << 7,
    Native      = 1 << 8,
    Volatile    = 1 << 9,
    Transient   = 1 << 10,
    Bridge      = 1 << 11,  ///< JVM bridge method (compiler-generated)
    Synthetic   = 1 << 12,
    VarArgs     = 1 << 13,
    Strict      = 1 << 14,  ///< JVM strictfp
    Sealed      = 1 << 15,  ///< CLR sealed / Kotlin sealed
    Override    = 1 << 16,  ///< CLR virtual override
    Virtual     = 1 << 17,
    Extern      = 1 << 18,  ///< CLR extern (P/Invoke)
    Unsafe      = 1 << 19,  ///< CLR unsafe
    Readonly    = 1 << 20,  ///< CLR readonly struct
};

inline BcAccess operator|(BcAccess a, BcAccess b) {
    return static_cast<BcAccess>(static_cast<uint32_t>(a) | static_cast<uint32_t>(b));
}
inline BcAccess operator&(BcAccess a, BcAccess b) {
    return static_cast<BcAccess>(static_cast<uint32_t>(a) & static_cast<uint32_t>(b));
}
inline bool hasFlag(BcAccess flags, BcAccess flag) {
    return (static_cast<uint32_t>(flags) & static_cast<uint32_t>(flag)) != 0;
}

// ─── Annotation / attribute ───────────────────────────────────────────────────

/** One key=value entry in an annotation. */
struct BcAnnotationValue {
    enum class Kind { Int, Float, String, Bool, Enum, Array, Annotation, Type };
    Kind        kind  = Kind::String;
    std::string stringValue;
    int64_t     intValue   = 0;
    double      floatValue = 0.0;
    bool        boolValue  = false;
    std::vector<BcAnnotationValue> arrayValue;
    std::string enumTypeName, enumConstant;
};

/**
 * A Java annotation or .NET attribute attached to a class, field, or method.
 */
struct BcAnnotation {
    std::string typeName;   ///< "java/lang/Override", "System.ObsoleteAttribute"
    bool        isVisible  = true;   ///< Runtime-visible vs. compile-time only
    std::map<std::string, BcAnnotationValue> elements;
};

// ─── Local variable ───────────────────────────────────────────────────────────

struct BcLocalVar {
    uint32_t    index  = 0;         ///< JVM/Wasm local slot index
    std::string name;               ///< From debug info ("count", "sb", etc.)
    BcType      type;
    uint32_t    startOffset = 0;    ///< Bytecode offset where this var is live
    uint32_t    endOffset   = 0;    ///< Bytecode offset where liveness ends (exclusive)
    bool        isParam     = false;///< Is this a method parameter?
};

// ─── BcField ──────────────────────────────────────────────────────────────────

struct BcField {
    std::string name;
    BcType      type;
    BcAccess    access  = BcAccess::None;
    std::optional<int64_t>  constantIntValue;    ///< For static final int fields
    std::optional<double>   constantFltValue;    ///< For static final float fields
    std::optional<std::string> constantStrValue; ///< For static final String fields
    std::vector<BcAnnotation> annotations;
    std::string signature;   ///< Generic signature (JVM Signature attribute)
};

// ─── BcMethod ─────────────────────────────────────────────────────────────────

struct BcMethod {
    std::string name;
    BcFuncType  descriptor;
    BcAccess    access       = BcAccess::None;
    bool        isConstructor= false;  ///< <init> / .ctor
    bool        isStaticInit = false;  ///< <clinit> / .cctor
    bool        isAbstract   = false;
    bool        isNative     = false;

    // Generic type parameters: "T extends Comparable<T>"
    std::vector<std::string> typeParams;

    // Parameter names (from debug / LocalVariableTable).
    std::vector<std::string> paramNames;

    // Local variable table.
    std::vector<BcLocalVar>  locals;

    // Control-flow graph (empty for abstract / native methods).
    BcCFG cfg;

    // Exception declarations (Java throws clause).
    std::vector<std::string> throwsList;

    // Annotations.
    std::vector<BcAnnotation>              annotations;
    std::vector<std::vector<BcAnnotation>> paramAnnotations; ///< Per parameter

    // Generic signature ("(Ljava/util/List<TE;>;)V").
    std::string signature;

    // Maximum stack depth and local count (JVM Code attribute).
    uint16_t maxStack  = 0;
    uint16_t maxLocals = 0;

    // For Kotlin / Scala: default parameter bitmask.
    uint64_t defaultParamMask = 0;
};

// ─── BcClass ──────────────────────────────────────────────────────────────────

struct BcClass {
    std::string name;           ///< Simple name: "String"
    std::string fqName;         ///< Fully-qualified: "java/lang/String" or "System.String"
    std::string packageName;    ///< "java/lang" or "System"
    std::string outerClass;     ///< For inner/nested classes

    // Generics
    std::vector<std::string> typeParams;    ///< "T", "E extends Comparable<E>"

    // Hierarchy
    std::optional<BcType> superClass;       ///< nullopt ↔ no explicit superclass
    std::vector<BcType>   interfaces;

    // Members
    std::vector<BcField>  fields;
    std::vector<BcMethod> methods;

    // Modifiers
    BcAccess access     = BcAccess::None;
    bool isInterface    = false;
    bool isAbstract     = false;
    bool isEnum         = false;
    bool isRecord       = false;   ///< Java 16+ records / C# record types
    bool isAnnotation   = false;   ///< Java @interface
    bool isModule       = false;   ///< Java 9+ module-info

    // Annotations on the class
    std::vector<BcAnnotation> annotations;

    // Generic signature
    std::string signature;

    // Source file name (debug info)
    std::string sourceFile;
    int32_t     sourceVersion = -1;  ///< JVM major version or CLR metadata version

    // Enum constants (ordered)
    std::vector<std::string> enumConstants;

    // Helper queries
    BcMethod*       findMethod(const std::string& name, const std::string& desc = "");
    const BcMethod* findMethod(const std::string& name, const std::string& desc = "") const;
    BcField*        findField(const std::string& name);
    const BcField*  findField(const std::string& name) const;
};

// ─── BcModule ─────────────────────────────────────────────────────────────────

/**
 * @brief BcModule — the top-level managed-language IR container.
 *
 * One BcModule per decompiled compilation unit (e.g. one .jar, one .dll,
 * one .pyc, one .wasm, one .lua).  May reference external types via the
 * `externalRefs` map (class name → source language import form).
 */
class BcModule {
public:
    BcModule() : name_(""), lang_(SourceLang::Unknown) {}
    explicit BcModule(std::string name, SourceLang lang = SourceLang::Unknown);

    const std::string& name()       const { return name_; }
    void               setName(const std::string& n) { name_ = n; }
    SourceLang         sourceLang() const { return lang_; }
    void               setSourceLang(SourceLang l) { lang_ = l; }

    // Class management
    BcClass&       addClass(BcClass cls);
    BcClass*       findClass(const std::string& fqName);
    const BcClass* findClass(const std::string& fqName) const;
    const std::vector<BcClass>& classes() const { return classes_; }
    std::vector<BcClass>&       classes()       { return classes_; }

    // String pool (shared storage for string constants)
    uint32_t    internString(const std::string& s);
    const std::string& string(uint32_t idx) const;
    uint32_t    stringCount() const { return static_cast<uint32_t>(stringPool_.size()); }

    // External type references (for import statements)
    void addExternalRef(const std::string& fqName, const std::string& importForm);
    const std::unordered_map<std::string, std::string>& externalRefs() const { return externalRefs_; }

    // Module-level metadata (CLR assembly attributes, Java Manifest, etc.)
    std::string version;
    std::string targetFramework;  ///< e.g. "net8.0", "java 17"
    std::vector<BcAnnotation> moduleAnnotations;

    // Validation
    bool verify(std::string& error) const;

private:
    std::string           name_;
    SourceLang            lang_;
    std::vector<BcClass>  classes_;
    std::vector<std::string> stringPool_;
    std::unordered_map<std::string, uint32_t> stringIndex_;
    std::unordered_map<std::string, std::string> externalRefs_;
};

} // namespace bc_module
} // namespace retdec

#endif // RETDEC_BC_MODULE_BC_MODULE_H
