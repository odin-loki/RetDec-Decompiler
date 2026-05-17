/**
 * @file src/java_emitter/java_file_emitter.cpp
 * @brief Top-level Java file emitter implementation.
 */

#include "retdec/java_emitter/java_file_emitter.h"

#include <fstream>
#include <sstream>

namespace retdec {
namespace java_emitter {

using namespace bc_module;
using namespace jvm_reconstruct;

// ─── JavaFileEmitter ─────────────────────────────────────────────────────────

JavaFileEmitter::JavaFileEmitter(FileEmitOptions opts)
    : opts_(std::move(opts)) {}

// ─── Package / path helpers ───────────────────────────────────────────────────

std::string JavaFileEmitter::packageOf(const BcClass& cls) {
    std::string pkg = cls.packageName;
    // Normalize slashes to dots.
    for (char& c : pkg) if (c == '/') c = '.';
    return pkg;
}

std::string JavaFileEmitter::toPathSeparator(const std::string& dotted) {
    std::string out = dotted;
    for (char& c : out) if (c == '.') c = '/';
    return out;
}

std::string JavaFileEmitter::relativePathOf(const BcClass& cls) {
    std::string pkg = packageOf(cls);
    std::string name = cls.name.empty() ? "Unknown" : cls.name;
    if (pkg.empty()) return name + ".java";
    return toPathSeparator(pkg) + "/" + name + ".java";
}

// ─── Reconstruction ───────────────────────────────────────────────────────────

std::unordered_map<std::string, ReconstructResult>
JavaFileEmitter::reconstructClass(BcClass& cls) const {
    std::unordered_map<std::string, ReconstructResult> results;
    if (!opts_.runReconstruction) return results;

    JvmReconstructor rec(opts_.reconOpts);
    for (auto& method : cls.methods) {
        if (method.cfg.blocks().empty()) continue;
        results[method.name] = rec.reconstruct(method);
    }
    return results;
}

// ─── File assembly ────────────────────────────────────────────────────────────

std::string JavaFileEmitter::assembleFile(const std::string& packageName,
                                           const ImportSet& imports,
                                           const std::string& classBody) const {
    std::ostringstream out;

    if (opts_.emitHeader) {
        out << "// Decompiled by " << opts_.version << "\n";
        out << "// Source language: Java\n";
        out << "\n";
    }

    if (!packageName.empty())
        out << "package " << packageName << ";\n\n";

    auto importLines = imports.importLines();
    if (!importLines.empty()) {
        for (const auto& line : importLines)
            out << line << "\n";
        out << "\n";
    }

    out << classBody;
    return out.str();
}

// ─── emitClass ───────────────────────────────────────────────────────────────

JavaFileResult JavaFileEmitter::emitClass(const BcClass& cls) {
    JavaFileResult result;
    result.className    = cls.name;
    result.packageName  = packageOf(cls);
    result.relativePath = relativePathOf(cls);

    // Run reconstruction on a mutable copy.
    BcClass clsCopy = cls;
    auto reconMap = reconstructClass(clsCopy);

    // Build import set for this file.
    std::string pkg = result.packageName;
    ImportSet imports(pkg, cls.name);
    JavaTypePrinter tyPrinter(imports);

    // Emit the class body.
    JavaClassEmitter classEmit(imports, tyPrinter, &reconMap, opts_.classOpts);
    CodeWriter writer;
    classEmit.emitClass(clsCopy, writer);

    result.source = assembleFile(pkg, imports, writer.str());
    return result;
}

// ─── emitModule ───────────────────────────────────────────────────────────────

JavaModuleResult JavaFileEmitter::emitModule(const BcModule& module) {
    JavaModuleResult result;

    for (const auto& cls : module.classes()) {
        // Skip inner classes (they have a '$' in their name); they'll be emitted
        // as part of their enclosing class.
        if (cls.name.find('$') != std::string::npos) continue;

        JavaFileResult fileResult = emitClass(cls);
        if (fileResult.hasErrors) ++result.totalErrors;
        result.totalWarnings += static_cast<int>(fileResult.warnings.size());
        result.files.push_back(std::move(fileResult));
    }

    return result;
}

// ─── writeFiles ──────────────────────────────────────────────────────────────

int JavaFileEmitter::writeFiles(const JavaModuleResult& result,
                                 const std::filesystem::path& outputRoot) const {
    int written = 0;
    for (const auto& file : result.files) {
        std::filesystem::path outPath = outputRoot / file.relativePath;
        std::error_code ec;
        std::filesystem::create_directories(outPath.parent_path(), ec);
        if (ec) continue;

        std::ofstream ofs(outPath);
        if (!ofs.is_open()) continue;
        ofs << file.source;
        ++written;
    }
    return written;
}

} // namespace java_emitter
} // namespace retdec
