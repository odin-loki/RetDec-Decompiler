/**
 * @file src/kotlin_emitter/kotlin_file_emitter.cpp
 * @brief Top-level Kotlin file emitter — orchestrates per-file .kt emission.
 */

#include "retdec/kotlin_emitter/kotlin_file_emitter.h"

#include <algorithm>
#include <cstring>
#include <fstream>
#include <map>
#include <sstream>

namespace retdec {
namespace kotlin_emitter {

// ─── KtImportSet ─────────────────────────────────────────────────────────────

KtImportSet::KtImportSet(const std::string& currentPackage)
    : currentPackage_(currentPackage) {}

bool KtImportSet::isKotlinImplicit(const std::string& fqName) const {
    // kotlin.*, kotlin.collections.*, kotlin.sequences.*, kotlin.text.*,
    // kotlin.io.*, kotlin.ranges.*, kotlin.jvm.*, java.lang.*
    static const char* const implicitPkgs[] = {
        "kotlin.",
        "kotlin.collections.",
        "kotlin.sequences.",
        "kotlin.text.",
        "kotlin.io.",
        "kotlin.ranges.",
        "kotlin.annotation.",
        "kotlin.reflect.",
        "kotlin.comparisons.",
        "java.lang.",
    };
    for (auto* pkg : implicitPkgs) {
        if (fqName.rfind(pkg, 0) == 0) {
            // Make sure it's a direct member (no further dots after the package)
            std::string rest = fqName.substr(std::strlen(pkg));
            if (rest.find('.') == std::string::npos)
                return true;
        }
    }
    return false;
}

bool KtImportSet::isImplicit(const std::string& fqName) const {
    if (isKotlinImplicit(fqName)) return true;
    // Same package
    if (!currentPackage_.empty()) {
        std::string pkg = currentPackage_ + ".";
        if (fqName.rfind(pkg, 0) == 0) {
            std::string rest = fqName.substr(pkg.size());
            if (rest.find('.') == std::string::npos)
                return true;
        }
    }
    return false;
}

std::string KtImportSet::simpleName(const std::string& fqName) const {
    auto pos = fqName.rfind('.');
    if (pos != std::string::npos) return fqName.substr(pos + 1);
    return fqName;
}

std::string KtImportSet::require(const std::string& fqName) {
    if (fqName.empty()) return "Any";
    // Normalise to dot-separated
    std::string dotName = fqName;
    std::replace(dotName.begin(), dotName.end(), '/', '.');

    if (isImplicit(dotName)) return simpleName(dotName);

    auto it = fqToShort_.find(dotName);
    if (it != fqToShort_.end()) return it->second;

    std::string simple = simpleName(dotName);
    // Check for collision
    auto nit = nameToFq_.find(simple);
    if (nit == nameToFq_.end()) {
        nameToFq_[simple]   = dotName;
        fqToShort_[dotName] = simple;
        return simple;
    }
    if (nit->second == dotName) return simple;
    // Collision — use fully qualified name (no import)
    return dotName;
}

std::vector<std::string> KtImportSet::importLines() const {
    std::vector<std::string> lines;
    lines.reserve(fqToShort_.size());
    for (const auto& [fq, _] : fqToShort_)
        lines.push_back("import " + fq);
    std::sort(lines.begin(), lines.end());
    return lines;
}

bool KtImportSet::empty() const { return fqToShort_.empty(); }

// ─── KotlinFileEmitter ────────────────────────────────────────────────────────

KotlinFileEmitter::KotlinFileEmitter(const KtFileEmitOptions& opts)
    : opts_(opts), emitter_(opts.emitterOpts) {}

std::unordered_map<std::string, ReconstructResult>
KotlinFileEmitter::reconstructMethods(const BcClass& cls) {
    std::unordered_map<std::string, ReconstructResult> results;
    JvmReconstructor recon(opts_.reconOpts);
    for (const auto& method : cls.methods) {
        if (method.cfg.blocks().empty()) continue;
        BcMethod mutableMethod = method;
        ReconstructResult res = recon.reconstruct(mutableMethod);
        std::string key = method.name + method.descriptor.jvmDescriptor();
        results[key] = std::move(res);
    }
    return results;
}

KtFileResult KotlinFileEmitter::assembleFile(const KtClass& ktClass,
                                              const std::string& body) {
    KtFileResult result;
    result.success        = true;
    result.sourceFileName = sourceFileName(ktClass);
    result.packageName    = ktClass.packageName;

    std::ostringstream out;

    // File header comment
    out << "// Decompiled by RetDec Kotlin emitter\n";
    out << "// Original class: " << ktClass.fqName << "\n\n";

    // Package declaration
    if (!ktClass.packageName.empty())
        out << "package " << ktClass.packageName << "\n\n";

    // The body already has the class text; split off imports if needed.
    // (We could run a two-pass emit to collect imports, but for now we
    //  emit without imports for external types — they use FQ names.)
    out << body << "\n";

    result.sourceCode = out.str();
    return result;
}

KtFileResult KotlinFileEmitter::emitClass(const BcClass& cls) {
    KtFileResult result;

    // Check if Kotlin
    if (!KotlinMetadataDetector::isKotlin(cls)) {
        result.error = "Not a Kotlin class";
        result.success = false;
        return result;
    }

    KotlinClassMetadata meta = KotlinMetadataDetector::detect(cls);
    if (!meta.isValid) {
        result.error = "Failed to decode @kotlin.Metadata: " + meta.error;
        result.success = false;
        return result;
    }

    KtClassReconstructor reconstructor;
    KtClass ktClass = reconstructor.reconstruct(cls, meta);

    auto recon = reconstructMethods(cls);

    CodeWriter writer;
    emitter_.emitClass(ktClass, recon, writer);
    std::string body = writer.str();

    return assembleFile(ktClass, body);
}

KtModuleResult KotlinFileEmitter::emitModule(const BcModule& module) {
    KtModuleResult result;

    for (const auto& cls : module.classes()) {
        if (KotlinMetadataDetector::isKotlin(cls)) {
            ++result.kotlinCount;
            KtFileResult fileResult = emitClass(cls);
            result.files.push_back(std::move(fileResult));
        } else {
            ++result.javaCount;
            // Java fallback: emit a stub .java file
            if (opts_.emitJavaFallback) {
                KtFileResult stub;
                stub.success        = true;
                stub.sourceFileName = cls.name + ".java";
                stub.packageName    = cls.packageName;
                stub.sourceCode     = "// " + cls.fqName + " (not Kotlin)\n";
                result.files.push_back(std::move(stub));
            }
        }
    }

    if (opts_.writeFiles)
        writeFiles(result);

    return result;
}

void KotlinFileEmitter::writeFiles(const KtModuleResult& result) {
    for (const auto& f : result.files) {
        if (!f.success) continue;
        fs::path out = outputPath(f.packageName, f.sourceFileName);
        fs::create_directories(out.parent_path());
        std::ofstream ofs(out);
        if (ofs.is_open())
            ofs << f.sourceCode;
    }
}

fs::path KotlinFileEmitter::outputPath(const std::string& packageName,
                                        const std::string& fileName) const {
    fs::path base = opts_.outputDir;
    if (opts_.preservePackageLayout && !packageName.empty()) {
        std::string pkg = packageName;
        std::replace(pkg.begin(), pkg.end(), '.', '/');
        base /= pkg;
    }
    return base / fileName;
}

std::string KotlinFileEmitter::sourceFileName(const KtClass& ktClass) const {
    if (!ktClass.sourceFile.empty()) return ktClass.sourceFile;
    return ktClass.name + ".kt";
}

} // namespace kotlin_emitter
} // namespace retdec
