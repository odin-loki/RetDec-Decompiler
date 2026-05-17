/**
 * @file include/retdec/kotlin_emitter/kotlin_type_system.h
 * @brief Kotlin type reconstruction from BcClass + KotlinClassMetadata.
 *
 * This module bridges the JVM bytecode view (BcClass) with the rich Kotlin
 * type system embedded in @kotlin.Metadata.  The output is a `KotlinClass`
 * structure that the emitter can directly render as .kt source.
 *
 * ## Key Kotlin concepts reconstructed
 *
 * ### Nullable types
 *   `KotlinType::nullable` → append `?` to the type name in source.
 *   Extension receiver types are also nullable-aware.
 *
 * ### Data classes
 *   Detected from `KotlinClassFlags::isData`.  The primary constructor
 *   parameters are the component() properties (those with `isVal`).
 *   The emitter generates `data class Foo(val x: Int, val y: String)`.
 *
 * ### Sealed classes
 *   Detected from `KotlinClassFlags::isSealed`.  Sealed subclasses are
 *   listed in `KotlinClassMetadata::sealedSubclasses`.
 *   Emitted as `sealed class Foo` with inner subclasses.
 *
 * ### Object declarations and companion objects
 *   `isObject`  → `object Foo { … }`
 *   `isCompanion` → `companion object [Name] { … }` inside a class.
 *
 * ### Value classes (formerly inline)
 *   `isInline` (proto flag) → `@JvmInline value class Foo(val x: T)`
 *   Single property in the primary constructor.
 *
 * ### Extension functions and properties
 *   `KotlinFunction::receiverType` is non-null → `fun ReceiverType.name(…)`
 *   `KotlinProperty::receiverType` is non-null → `val ReceiverType.name: T`
 *
 * ### Coroutine suspend functions
 *   `KotlinFunction::isSuspend` → `suspend fun name(…): T`
 *   The JVM signature has an added `Continuation<T>` parameter which is
 *   removed when rendering Kotlin source.
 *
 * ### Inline functions with reified type params
 *   `KotlinFunction::isInline` + `KotlinTypeParam::isReified` →
 *   `inline fun <reified T> name(…): T`
 *
 * ### Operator overloads
 *   `KotlinFunction::isOperator` → `operator fun …`
 *   The operator name is used as-is (`plus`, `minus`, `invoke`, etc.)
 *
 * ### Infix functions
 *   `KotlinFunction::isInfix` → `infix fun …`
 *
 * ### when expressions
 *   Not directly reconstructed here; detected by the emitter from switch
 *   patterns in the BcCFG.  The type system module provides the sealed
 *   subclass list for exhaustive when detection.
 */

#ifndef RETDEC_KOTLIN_EMITTER_KOTLIN_TYPE_SYSTEM_H
#define RETDEC_KOTLIN_EMITTER_KOTLIN_TYPE_SYSTEM_H

#include "retdec/bc_module/bc_module.h"
#include "retdec/kotlin_emitter/kotlin_metadata.h"

#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace retdec {
namespace kotlin_emitter {

using namespace bc_module;

// ─── Kotlin visibility ────────────────────────────────────────────────────────

enum class KtVisibility { Local, Private, Protected, Internal, Public };

inline std::string visibilityStr(KtVisibility v) {
    switch (v) {
        case KtVisibility::Private:   return "private";
        case KtVisibility::Protected: return "protected";
        case KtVisibility::Internal:  return "internal";
        case KtVisibility::Public:    return "";  // implicit in Kotlin
        default:                      return "";
    }
}

// ─── Kotlin reconstructed property ───────────────────────────────────────────

struct KtProperty {
    std::string name;
    std::string type;           ///< Rendered type string (with '?' if nullable)
    std::string receiverType;   ///< Extension receiver, or empty
    bool        isVar       = false;
    bool        isLateinit  = false;
    bool        isConst     = false;
    bool        isDelegated = false;
    bool        isOverride  = false;
    bool        isAbstract  = false;
    bool        isOpen      = false;
    bool        isPrimaryCtorParam = false; ///< Part of data/value class primary ctor
    KtVisibility visibility = KtVisibility::Public;
    std::string  initializer;   ///< Constant value expression (for const val)
    std::vector<std::string> annotations;
};

// ─── Kotlin reconstructed function ───────────────────────────────────────────

struct KtParam {
    std::string name;
    std::string type;
    bool        hasDefault  = false;
    bool        isVarArg    = false;
    bool        isCrossInline = false;
    bool        isNoInline    = false;
    std::vector<std::string> annotations;
};

struct KtFunction {
    std::string name;
    std::string returnType;       ///< Including '?' if nullable
    std::string receiverType;     ///< Extension receiver, or empty
    std::vector<KtParam>       params;
    std::vector<std::string>   typeParams; ///< "T", "reified T : Foo", etc.

    bool isOperator  = false;
    bool isInfix     = false;
    bool isInline    = false;
    bool isTailrec   = false;
    bool isSuspend   = false;
    bool isOverride  = false;
    bool isAbstract  = false;
    bool isOpen      = false;
    bool isExternal  = false;
    bool isExpect    = false;    ///< multiplatform expect
    KtVisibility visibility = KtVisibility::Public;
    std::vector<std::string> annotations;

    /// The correlated BcMethod (for body emission).
    const BcMethod* bcMethod = nullptr;
};

// ─── Kotlin reconstructed class ───────────────────────────────────────────────

enum class KtClassKind {
    Class,
    DataClass,
    SealedClass,
    SealedInterface,
    ObjectDecl,
    CompanionObject,
    Interface,
    FunInterface,
    Enum,
    Annotation,
    ValueClass,     ///< @JvmInline value class
};

struct KtClass {
    std::string  name;
    std::string  fqName;
    std::string  packageName;
    KtClassKind  kind = KtClassKind::Class;
    KtVisibility visibility = KtVisibility::Public;

    bool isAbstract   = false;
    bool isOpen       = false;
    bool isSealed     = false;
    bool isExpect     = false;
    bool isExternal   = false;
    bool isInner      = false;

    // Generic type parameters.
    std::vector<std::string>  typeParams; ///< "T", "in T", "out T : Comparable<T>"

    // Hierarchy.
    std::optional<std::string> superClass;  ///< Rendered superclass name
    std::vector<std::string>   interfaces;  ///< Rendered interface names

    // Primary constructor parameters (for data/value classes).
    std::vector<KtProperty>    primaryCtorParams;

    // Members.
    std::vector<KtProperty>    properties;
    std::vector<KtFunction>    functions;

    // Enum entries.
    std::vector<std::string>   enumEntries;

    // Sealed subclass names (simple names, for when-expression completeness).
    std::vector<std::string>   sealedSubclasses;

    // Companion object (embedded in this class).
    std::unique_ptr<KtClass>   companion;

    // Nested/inner class names.
    std::vector<std::string>   nestedClasses;

    // Source file (from SourceFile annotation).
    std::string  sourceFile;

    // Annotations on the class.
    std::vector<std::string>   annotations;

    // The underlying BcClass for body emission.
    const BcClass* bcClass = nullptr;
};

// ─── Type renderer ────────────────────────────────────────────────────────────

/**
 * @brief Renders a KotlinType to a Kotlin source type string.
 *
 * Takes the string table from KotlinClassMetadata and uses it to look up
 * class names.  Produces idiomatic Kotlin type strings:
 *
 *   String            → "String"
 *   String?           → "String?"
 *   List<Int>         → "List<Int>"
 *   Map<String,Int?>  → "Map<String, Int?>"
 *   (Int) -> String   → "(Int) -> String"
 *   suspend (T) -> Unit → "suspend (T) -> Unit"
 *   *                 → "*"  (star projection)
 */
class KtTypeRenderer {
public:
    explicit KtTypeRenderer(const std::vector<std::string>& stringTable);

    std::string render(const KotlinType& type) const;
    std::string render(const std::shared_ptr<KotlinType>& type) const;

private:
    const std::vector<std::string>& strings_;

    std::string renderArg(const KotlinTypeArg& arg) const;
    std::string kotlinName(const std::string& jvmName) const;
};

// ─── Class reconstructor ──────────────────────────────────────────────────────

/**
 * @brief Reconstructs a KtClass from a BcClass + KotlinClassMetadata.
 *
 * Correlates BcMethod entries with KotlinFunction entries using the JVM
 * method descriptor, resolves type parameters, and builds the full
 * KtClass ready for emission.
 */
class KtClassReconstructor {
public:
    /**
     * @brief Reconstruct a KtClass from BcClass + metadata.
     *
     * @param cls      The BcClass from the JVM parser.
     * @param meta     The decoded Kotlin metadata (from KotlinMetadataDetector).
     */
    KtClass reconstruct(const BcClass& cls, const KotlinClassMetadata& meta);

private:
    // Classify the class kind from metadata flags.
    static KtClassKind classifyKind(const KotlinClassFlags& flags);

    // Convert visibility int to KtVisibility.
    static KtVisibility toVisibility(int v);

    // Render type parameters from KotlinTypeParam list.
    static std::vector<std::string>
        renderTypeParams(const std::vector<KotlinTypeParam>& params,
                          const KtTypeRenderer& renderer);

    // Correlate a KotlinFunction with a BcMethod.
    static const BcMethod*
        findBcMethod(const BcClass& cls, const KotlinFunction& kfun);

    // Build a KtFunction from a KotlinFunction.
    static KtFunction
        buildFunction(const KotlinFunction& kfun,
                       const BcClass& cls,
                       const KtTypeRenderer& renderer);

    // Build a KtProperty from a KotlinProperty.
    static KtProperty
        buildProperty(const KotlinProperty& kprop,
                       const KtTypeRenderer& renderer);

    // Determine which properties are primary constructor params (data/value class).
    static std::vector<KtProperty>
        extractPrimaryCtorParams(const std::vector<KotlinProperty>& props,
                                  bool isDataClass, bool isValueClass,
                                  const KtTypeRenderer& renderer);

    // Remove synthetic continuation parameter from suspend functions.
    static void removeContinuationParam(KtFunction& fn);
};

} // namespace kotlin_emitter
} // namespace retdec

#endif // RETDEC_KOTLIN_EMITTER_KOTLIN_TYPE_SYSTEM_H
