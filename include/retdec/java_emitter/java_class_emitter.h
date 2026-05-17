/**
 * @file include/retdec/java_emitter/java_class_emitter.h
 * @brief Java class/interface/enum/record/annotation emitter.
 *
 * Emits the complete declaration of a BcClass as valid Java source, including:
 *
 *   - Class modifiers (public, protected, private, abstract, final, sealed, …)
 *   - Generic type parameters with bounds
 *   - extends / implements clauses
 *   - Field declarations with initializers
 *   - Method declarations:
 *       - Modifiers + generic type params
 *       - Parameter list with names and types
 *       - throws clause
 *       - Method body (delegated to JavaStmtEmitter)
 *   - Constructor declarations
 *   - Static initializer blocks
 *   - Inner / nested class declarations (recursive)
 *   - Enum constant declarations with optional bodies
 *   - Record component declarations (Java 16+)
 *   - Annotation element declarations
 *   - Annotation usage on fields, methods, classes
 *
 * ## Quality targets
 *
 * - All emitted modifiers in canonical javac order:
 *     public protected private abstract static final transient volatile
 *     synchronized native strictfp
 * - Generic signatures use the exact type parameters from BcMethod::typeParams
 *   / BcClass::typeParams (if present from Signature attribute).
 * - Constructors are emitted as `ClassName(...)` not `void <init>(...)`.
 */

#ifndef RETDEC_JAVA_EMITTER_JAVA_CLASS_EMITTER_H
#define RETDEC_JAVA_EMITTER_JAVA_CLASS_EMITTER_H

#include "retdec/bc_module/bc_module.h"
#include "retdec/java_emitter/java_stmt_emitter.h"
#include "retdec/java_emitter/java_type_printer.h"
#include "retdec/jvm_reconstruct/jvm_reconstruct.h"

#include <string>
#include <unordered_map>
#include <vector>

namespace retdec {
namespace java_emitter {

// ─── Class emit options ───────────────────────────────────────────────────────

struct ClassEmitOptions {
    StmtEmitOptions stmtOpts;
    bool emitBridgeMethods   = false; ///< Suppress compiler-generated bridges
    bool emitSyntheticFields = false; ///< Suppress synthetic fields
    bool emitLineNumbers     = false; ///< Emit "// line N" comments
    bool emitOriginalNames   = true;  ///< Use names from debug info
    int  javaVersion         = 17;
};

// ─── Class emitter ───────────────────────────────────────────────────────────

/**
 * @brief Emits a complete Java class (or interface/enum/record/annotation)
 *        from a BcClass with optional reconstruction results.
 *
 * Each method body is emitted using a fresh `JavaStmtEmitter`.
 * Reconstruction results (if provided) supply local variable names and
 * pattern annotations; without them the emitter falls back to stack-slot names.
 */
class JavaClassEmitter {
public:
    /**
     * @param imports     Shared import set for the compilation unit.
     * @param tyPrinter   Type printer bound to `imports`.
     * @param reconMap    Optional map from method name → ReconstructResult.
     * @param opts        Emission options.
     */
    JavaClassEmitter(ImportSet& imports,
                     JavaTypePrinter& tyPrinter,
                     const std::unordered_map<std::string,
                                               ReconstructResult>* reconMap,
                     const ClassEmitOptions& opts = ClassEmitOptions{});

    /**
     * @brief Emit `cls` into `writer` at the given indent level.
     */
    void emitClass(const BcClass& cls, CodeWriter& writer, int depth = 0);

private:
    ImportSet&       imports_;
    JavaTypePrinter& tyPrinter_;
    const std::unordered_map<std::string, ReconstructResult>* reconMap_;
    ClassEmitOptions opts_;

    // ── Class-level emission ──────────────────────────────────────────────────

    void emitClassHeader(const BcClass& cls, CodeWriter& writer);
    void emitFields(const BcClass& cls, CodeWriter& writer);
    void emitMethods(const BcClass& cls, CodeWriter& writer);
    void emitInnerClasses(const BcClass& cls, CodeWriter& writer);
    void emitEnumConstants(const BcClass& cls, CodeWriter& writer);
    void emitRecordComponents(const BcClass& cls, CodeWriter& writer);

    // ── Field emission ────────────────────────────────────────────────────────

    void emitField(const BcField& field, const BcClass& cls,
                    CodeWriter& writer);

    // ── Method emission ───────────────────────────────────────────────────────

    void emitMethod(const BcMethod& method, const BcClass& cls,
                     CodeWriter& writer);
    void emitMethodSignature(const BcMethod& method, const BcClass& cls,
                              CodeWriter& writer);
    std::string buildParamList(const BcMethod& method,
                                const ReconstructResult* recon) const;

    // ── Annotation emission ───────────────────────────────────────────────────

    void emitAnnotations(const std::vector<BcAnnotation>& anns,
                          CodeWriter& writer);
    std::string emitAnnotation(const BcAnnotation& ann) const;
    std::string emitAnnotationValue(const BcAnnotationValue& val) const;

    // ── Modifier emission ─────────────────────────────────────────────────────

    std::string modifiersFor(BcAccess access, bool isInterface,
                              bool isField, bool isMethod) const;

    // ── Helpers ───────────────────────────────────────────────────────────────

    // Returns the javac-canonical modifier order string for the given flags.
    static std::string canonicalModifiers(BcAccess access,
                                           bool isInterface,
                                           bool isField,
                                           bool isMethod);

    // Returns the reconstruction result for a method (or nullptr if absent).
    const ReconstructResult* reconFor(const BcMethod& method) const;

    // Generate simple name for an anonymous class.
    static std::string anonClassName(uint32_t idx);
};

} // namespace java_emitter
} // namespace retdec

#endif // RETDEC_JAVA_EMITTER_JAVA_CLASS_EMITTER_H
