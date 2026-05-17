/**
 * @file include/retdec/kotlin_emitter/kotlin_emitter.h
 * @brief Kotlin source emitter — produces idiomatic .kt source.
 *
 * Takes a `KtClass` (from KtClassReconstructor) and emits syntactically
 * valid Kotlin source that kotlinc can compile.
 *
 * ## Kotlin constructs emitted
 *
 * ### Declarations
 *   class Foo(…)                 — regular class with primary constructor
 *   data class Foo(val x: T, …) — data class
 *   sealed class Foo             — sealed hierarchy
 *   object Foo                   — singleton object declaration
 *   companion object [Name] {…} — companion object
 *   interface Foo                — interface
 *   fun interface Foo {…}        — SAM functional interface
 *   enum class Foo {A, B, C}     — enum class
 *   annotation class Foo(…)      — annotation class
 *   @JvmInline value class Foo(val x: T) — value class
 *
 * ### Modifiers
 *   private / protected / internal (public is implicit)
 *   abstract / open / sealed / override / operator / infix / inline / tailrec
 *   suspend / external / expect / actual
 *   lateinit val/var / const val
 *
 * ### Generics
 *   fun <T : Comparable<T>> foo(x: T): T
 *   fun <reified T> bar(): T   (only in inline fun)
 *   class Foo<out T, in K : Any>
 *
 * ### Extension functions and properties
 *   fun String.repeat(n: Int): String
 *   val String.digits: Int
 *
 * ### Nullable types and null-safety operators
 *   x?.field                    — safe call
 *   x ?: default                — Elvis operator
 *   x!!                         — non-null assertion
 *
 * ### String templates
 *   "$name has ${list.size} items"
 *   Detected from StringBuilder + concatenation with non-string expressions.
 *
 * ### when expressions
 *   when (x) {
 *       is Circle -> x.area()
 *       is Square -> x.side * x.side
 *       else -> 0.0
 *   }
 *   Emitted for: sealed class hierarchies, enum when, integer switches.
 *
 * ### Scope functions
 *   Detected from: let { }, run { }, apply { }, also { }, with(x) { }.
 *
 * ### Operator calls
 *   Arithmetic (plus/minus/times/div/rem), comparison (compareTo),
 *   index (get/set), invoke, contains (in/!in), rangeTo (..),
 *   unary (unaryPlus/unaryMinus/not/inc/dec).
 */

#ifndef RETDEC_KOTLIN_EMITTER_KOTLIN_EMITTER_H
#define RETDEC_KOTLIN_EMITTER_KOTLIN_EMITTER_H

#include "retdec/java_emitter/java_stmt_emitter.h"
#include "retdec/kotlin_emitter/kotlin_type_system.h"
#include "retdec/jvm_reconstruct/jvm_reconstruct.h"

#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace retdec {
namespace kotlin_emitter {

using namespace java_emitter;
using namespace jvm_reconstruct;

// ─── Emitter options ──────────────────────────────────────────────────────────

struct KtEmitOptions {
    bool emitDataClasses        = true;
    bool emitSealedHierarchy    = true;
    bool emitObjectDeclarations = true;
    bool emitExtensionFunctions = true;
    bool emitSuspendFunctions   = true;
    bool emitStringTemplates    = true;
    bool emitWhenExpressions    = true;
    bool emitOperatorFunctions  = true;
    bool emitScopeFunctions     = true;
    bool emitLambdas            = true;
    int  kotlinVersion          = 17;   ///< Target JVM target (not Kotlin version)
};

// ─── Kotlin emitter ───────────────────────────────────────────────────────────

/**
 * @brief Emits a KtClass as idiomatic Kotlin source.
 */
class KotlinEmitter {
public:
    explicit KotlinEmitter(const KtEmitOptions& opts = KtEmitOptions{});

    /**
     * @brief Emit `ktClass` into a CodeWriter.
     *
     * @param ktClass   The reconstructed Kotlin class.
     * @param recon     Reconstruction results for method bodies (may be empty).
     * @param writer    Output buffer.
     */
    void emitClass(const KtClass& ktClass,
                    const std::unordered_map<std::string, ReconstructResult>& recon,
                    CodeWriter& writer);

private:
    KtEmitOptions opts_;

    // ── Class-level emission ──────────────────────────────────────────────────

    void emitClassHeader(const KtClass& cls, CodeWriter& writer);
    void emitPrimaryConstructor(const KtClass& cls, CodeWriter& writer);
    void emitBody(const KtClass& cls,
                   const std::unordered_map<std::string, ReconstructResult>& recon,
                   CodeWriter& writer);
    void emitCompanionObject(const KtClass& companion,
                              const std::unordered_map<std::string, ReconstructResult>& recon,
                              CodeWriter& writer);
    void emitEnum(const KtClass& cls, CodeWriter& writer);
    void emitSealed(const KtClass& cls,
                     const std::unordered_map<std::string, ReconstructResult>& recon,
                     CodeWriter& writer);

    // ── Property emission ─────────────────────────────────────────────────────

    void emitProperty(const KtProperty& prop, bool inPrimaryConstructor,
                       CodeWriter& writer);

    // ── Function emission ─────────────────────────────────────────────────────

    void emitFunction(const KtFunction& fn,
                       const std::unordered_map<std::string, ReconstructResult>& recon,
                       const KtClass& ownerClass,
                       CodeWriter& writer);

    // Emit the function signature line.
    std::string buildFunctionSignature(const KtFunction& fn) const;

    // Emit the function body using JVM reconstruction results.
    void emitFunctionBody(const KtFunction& fn,
                           const ReconstructResult& recon,
                           CodeWriter& writer);

    // ── Expression emission ────────────────────────────────────────────────────

    // Detect and render a string template from a concatenation pattern.
    std::string emitStringTemplate(const StringConcatPattern& pat,
                                    const ReconstructResult& recon) const;

    // Render a when expression from a switch pattern.
    std::string emitWhen(const std::string& subject,
                          const std::vector<std::pair<std::string,std::string>>& branches,
                          const std::string& elseBranch) const;

    // ── Annotation emission ────────────────────────────────────────────────────

    void emitAnnotations(const std::vector<std::string>& anns, CodeWriter& writer);

    // ── Modifier helpers ───────────────────────────────────────────────────────

    std::string classModifiers(const KtClass& cls) const;
    std::string functionModifiers(const KtFunction& fn) const;
    std::string propertyModifiers(const KtProperty& prop) const;
};

} // namespace kotlin_emitter
} // namespace retdec

#endif // RETDEC_KOTLIN_EMITTER_KOTLIN_EMITTER_H
