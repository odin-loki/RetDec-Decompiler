/**
 * @file include/retdec/kotlin_emitter/kotlin_file_emitter.h
 * @brief Top-level Kotlin file emitter — orchestrates per-file .kt emission.
 *
 * Kotlin classes that were originally in the same source file are identified
 * via the `@kotlin.jvm.internal.SourceDebugExtension` or the `SourceFile`
 * class file attribute.  This module groups `KtClass`es by source file and
 * assembles complete .kt files including:
 *
 *   1. Package declaration (`package foo.bar`)
 *   2. Import statements (collected via KtImportSet)
 *   3. Top-level function declarations (from File-kind metadata)
 *   4. Class declarations (via KotlinEmitter)
 *
 * ## KtImportSet
 *
 * Similar to `ImportSet` in java_emitter but Kotlin-aware:
 *   - Skips `kotlin.*` and `kotlin.collections.*` (always imported)
 *   - Skips `java.lang.*`
 *   - Handles star imports for kotlin.jvm annotations
 *   - Deduplicates and sorts alphabetically
 */

#ifndef RETDEC_KOTLIN_EMITTER_KOTLIN_FILE_EMITTER_H
#define RETDEC_KOTLIN_EMITTER_KOTLIN_FILE_EMITTER_H

#include "retdec/bc_module/bc_module.h"
#include "retdec/kotlin_emitter/kotlin_emitter.h"
#include "retdec/kotlin_emitter/kotlin_metadata.h"
#include "retdec/kotlin_emitter/kotlin_type_system.h"
#include "retdec/jvm_reconstruct/jvm_reconstruct.h"

#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace retdec {
namespace kotlin_emitter {

namespace fs = std::filesystem;
using namespace bc_module;
using namespace jvm_reconstruct;

// ─── Kotlin import set ────────────────────────────────────────────────────────

class KtImportSet {
public:
    explicit KtImportSet(const std::string& currentPackage = "");

    /// Register a fully-qualified Kotlin/Java type for import.
    /// Returns the short name to use in source (handles collisions).
    std::string require(const std::string& fqName);

    /// All sorted import lines (e.g. "import foo.bar.Baz").
    std::vector<std::string> importLines() const;

    /// True if no imports were added.
    bool empty() const;

private:
    std::string currentPackage_;
    std::map<std::string, std::string> nameToFq_;   ///< short→fq (resolved)
    std::map<std::string, std::string> fqToShort_;  ///< fq→short name

    bool isImplicit(const std::string& fqName) const;
    bool isKotlinImplicit(const std::string& fqName) const;
    std::string simpleName(const std::string& fqName) const;
};

// ─── Per-file result ──────────────────────────────────────────────────────────

struct KtFileResult {
    std::string          sourceFileName; ///< e.g. "Foo.kt"
    std::string          packageName;    ///< e.g. "com.example"
    std::string          sourceCode;     ///< Complete .kt file contents
    std::string          error;          ///< Non-empty on failure
    bool                 success = false;
};

// ─── Module result ────────────────────────────────────────────────────────────

struct KtModuleResult {
    std::vector<KtFileResult> files;
    int                       kotlinCount = 0; ///< # Kotlin classes detected
    int                       javaCount   = 0; ///< # non-Kotlin classes (→ .java)
};

// ─── File emit options ────────────────────────────────────────────────────────

struct KtFileEmitOptions {
    KtEmitOptions      emitterOpts;
    ReconstructOptions reconOpts;
    bool               writeFiles       = false;
    fs::path           outputDir;
    bool               preservePackageLayout = true;
    bool               emitJavaFallback = true; ///< Emit .java for non-Kotlin classes
};

// ─── Top-level file emitter ───────────────────────────────────────────────────

/**
 * @brief Orchestrates detection, reconstruction, and emission for an entire
 *        BcModule, splitting classes into .kt and .java files.
 */
class KotlinFileEmitter {
public:
    explicit KotlinFileEmitter(const KtFileEmitOptions& opts = KtFileEmitOptions{});

    /**
     * @brief Emit all classes in the BcModule.
     *
     * - Kotlin classes (detected by @kotlin.Metadata) → .kt
     * - Other JVM classes → .java (via java_emitter, if emitJavaFallback)
     */
    KtModuleResult emitModule(const BcModule& module);

    /**
     * @brief Emit a single BcClass.
     *
     * If it carries @kotlin.Metadata, emits Kotlin; otherwise Java.
     */
    KtFileResult emitClass(const BcClass& cls);

    /**
     * @brief Write emitted files to disk.
     */
    void writeFiles(const KtModuleResult& result);

private:
    KtFileEmitOptions opts_;
    KotlinEmitter     emitter_;

    // Reconstruct method bodies using JvmReconstructor.
    std::unordered_map<std::string, ReconstructResult>
        reconstructMethods(const BcClass& cls);

    // Assemble a complete .kt file from package + imports + body.
    KtFileResult assembleFile(const KtClass& ktClass,
                               const std::string& body);

    // Derive the output path from packageName and sourceFileName.
    fs::path outputPath(const std::string& packageName,
                         const std::string& sourceFileName) const;

    // Extract the source file name from KtClass or BcClass annotations.
    std::string sourceFileName(const KtClass& ktClass) const;
};

} // namespace kotlin_emitter
} // namespace retdec

#endif // RETDEC_KOTLIN_EMITTER_KOTLIN_FILE_EMITTER_H
