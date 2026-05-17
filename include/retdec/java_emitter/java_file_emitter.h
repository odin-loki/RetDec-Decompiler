/**
 * @file include/retdec/java_emitter/java_file_emitter.h
 * @brief Top-level Java file emitter — produces compilable .java source files.
 *
 * ## File structure
 *
 *   // Decompiled by RetDec <version>
 *   package com.example;
 *
 *   import java.util.ArrayList;
 *   import java.util.List;
 *
 *   public class Foo implements Bar<String> {
 *       …
 *   }
 *
 * ## Workflow
 *
 *   1. Call `emitFile()` with a BcClass that is a top-level class.
 *   2. The emitter runs `JvmReconstructor` on every method in the class.
 *   3. All required types are registered in an `ImportSet`.
 *   4. The `JavaClassEmitter` emits the class body.
 *   5. The file emitter prepends the package declaration and sorted imports.
 *   6. The result is a `JavaFileResult` with the complete source text.
 *
 * ## File writing
 *
 *   Call `writeFiles()` to write the .java files to disk under a given root.
 *   The directory structure mirrors the package (e.g. com/example/Foo.java).
 */

#ifndef RETDEC_JAVA_EMITTER_JAVA_FILE_EMITTER_H
#define RETDEC_JAVA_EMITTER_JAVA_FILE_EMITTER_H

#include "retdec/bc_module/bc_module.h"
#include "retdec/java_emitter/java_class_emitter.h"
#include "retdec/java_emitter/java_type_printer.h"
#include "retdec/jvm_reconstruct/jvm_reconstruct.h"

#include <filesystem>
#include <string>
#include <unordered_map>
#include <vector>

namespace retdec {
namespace java_emitter {

// ─── File emit options ────────────────────────────────────────────────────────

struct FileEmitOptions {
    ClassEmitOptions classOpts;
    ReconstructOptions reconOpts;        ///< Options for JvmReconstructor
    bool runReconstruction  = true;      ///< Run JvmReconstructor before emit
    bool emitHeader         = true;      ///< Emit "// Decompiled by RetDec" comment
    std::string version     = "RetDec";  ///< Version string in header comment
};

// ─── Per-file result ─────────────────────────────────────────────────────────

struct JavaFileResult {
    std::string className;      ///< Simple class name: "Foo"
    std::string packageName;    ///< "com.example" (dot-separated)
    std::string relativePath;   ///< "com/example/Foo.java"
    std::string source;         ///< Complete .java source text
    bool        hasErrors = false;
    std::vector<std::string> warnings;
};

// ─── Module emit result ───────────────────────────────────────────────────────

struct JavaModuleResult {
    std::vector<JavaFileResult> files;
    int totalErrors   = 0;
    int totalWarnings = 0;
};

// ─── File emitter ─────────────────────────────────────────────────────────────

/**
 * @brief Emits .java source files from a BcModule or individual BcClass.
 *
 * ## Usage
 *
 *   JavaFileEmitter emitter;
 *   auto result = emitter.emitModule(module);
 *   emitter.writeFiles(result, "output/");
 */
class JavaFileEmitter {
public:
    explicit JavaFileEmitter(FileEmitOptions opts = FileEmitOptions{});

    /**
     * @brief Emit a single BcClass as a .java file.
     */
    JavaFileResult emitClass(const BcClass& cls);

    /**
     * @brief Emit all top-level classes in a BcModule.
     */
    JavaModuleResult emitModule(const BcModule& module);

    /**
     * @brief Write all emitted files to disk under `outputRoot`.
     *
     * Creates subdirectories for packages automatically.
     *
     * @return Number of files successfully written.
     */
    int writeFiles(const JavaModuleResult& result,
                    const std::filesystem::path& outputRoot) const;

private:
    FileEmitOptions opts_;

    // Run JvmReconstructor on all methods of `cls`.
    // Returns a method-name → ReconstructResult map.
    std::unordered_map<std::string, ReconstructResult>
        reconstructClass(BcClass& cls) const;

    // Assemble the complete .java source from package, imports, and class body.
    std::string assembleFile(const std::string& packageName,
                              const ImportSet& imports,
                              const std::string& classBody) const;

    // Derive the package and relative path from a BcClass.
    static std::string packageOf(const BcClass& cls);
    static std::string relativePathOf(const BcClass& cls);
    static std::string toPathSeparator(const std::string& dotted);
};

} // namespace java_emitter
} // namespace retdec

#endif // RETDEC_JAVA_EMITTER_JAVA_FILE_EMITTER_H
