/**
 * @file include/retdec/jvm_parser/jvm_jar_reader.h
 * @brief JAR/WAR/EAR reader: ZIP enumeration → BcModule.
 *
 * ## JAR handling
 *   - Enumerates all .class entries in the ZIP central directory.
 *   - Multi-release JARs (MRJAR, Java 9+): entries under
 *     META-INF/versions/N/ override base entries for version N+.
 *   - Spring Boot "fat JARs": classes nested in BOOT-INF/classes/ or
 *     WEB-INF/classes/; JARs nested in BOOT-INF/lib/ or WEB-INF/lib/.
 *   - Module path: module-info.class provides module name / exports / requires.
 *
 * ## Cross-class type resolution
 *   After all classes are parsed, `TypeResolver` builds a cross-class type
 *   graph so that:
 *   - Field types from other classes are resolved to BcType (not just
 *     a string descriptor).
 *   - `instanceof` chains and virtual dispatch targets are annotated.
 *   - Anonymous inner classes are linked to their enclosing method.
 */

#ifndef RETDEC_JVM_PARSER_JVM_JAR_READER_H
#define RETDEC_JVM_PARSER_JVM_JAR_READER_H

#include "retdec/jvm_parser/jvm_class_parser.h"
#include "retdec/bc_module/bc_module.h"

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace retdec {
namespace jvm_parser {

// ─── JAR entry ────────────────────────────────────────────────────────────────

struct JarEntry {
    std::string           path;      ///< Entry path within ZIP (e.g. "com/example/Foo.class")
    std::vector<uint8_t>  data;      ///< Decompressed (DEFLATE or stored) bytes
    int                   version = 0; ///< Multi-release version (0 = base)
};

// ─── JAR read options ─────────────────────────────────────────────────────────

struct JarReadOptions {
    int  targetJavaVersion = 21;       ///< Preferred runtime version for MRJAR
    bool parseBoot         = true;     ///< Parse Spring Boot nested JARs
    bool parseNestedJars   = false;    ///< Recursively parse nested lib JARs
    bool resolveTypes      = true;     ///< Run cross-class type resolver
    JvmParseOptions classOpts;
};

// ─── JAR read result ──────────────────────────────────────────────────────────

struct JarReadResult {
    bool        ok    = false;
    std::string error;

    bc_module::BcModule module{"", bc_module::SourceLang::Java};

    uint32_t classesFound  = 0;
    uint32_t classesParsed = 0;
    uint32_t parseErrors   = 0;
    std::vector<std::string> errorList;   ///< class path → error message
};

// ─── JarReader ────────────────────────────────────────────────────────────────

/**
 * @brief Reads a JAR/WAR/EAR ZIP archive and produces a BcModule.
 *
 * Implementation uses the built-in miniz / zlib DEFLATE decompressor.
 * No external ZIP library is required.
 */
class JarReader {
public:
    explicit JarReader(JarReadOptions opts = JarReadOptions{});

    /// Read a JAR from raw bytes.
    JarReadResult read(const uint8_t* data, size_t size);

    /// Read a JAR from a byte vector.
    JarReadResult read(const std::vector<uint8_t>& data);

    /// Enumerate ZIP entries without parsing class files (for testing).
    std::vector<JarEntry> listEntries(const uint8_t* data, size_t size);

private:
    JarReadOptions opts_;

    bool isClassEntry(const std::string& path) const;
    bool isNestedJar(const std::string& path) const;
    int  multiReleaseVersion(const std::string& path) const;
    std::string stripMRPrefix(const std::string& path) const;

    void resolveTypes(bc_module::BcModule& mod,
                      const std::vector<JvmParseResult>& results);
};

// ─── Cross-class type resolver ────────────────────────────────────────────────

/**
 * @brief Resolves field/method types across class boundaries.
 *
 * After all classes in a JAR are parsed into BcClass objects,
 * TypeResolver fills in:
 *   - Superclass BcType for each BcClass (not just name string).
 *   - Interface BcType list.
 *   - Anonymous/inner class links to their enclosing method.
 *   - External class references not found in the JAR are marked as
 *     Class("fully/qualified/Name") with `externalRef` noted in BcModule.
 */
class TypeResolver {
public:
    explicit TypeResolver(bc_module::BcModule& mod);

    void resolve(const std::vector<JvmParseResult>& results);

private:
    bc_module::BcModule& mod_;

    bc_module::BcType resolveType(const std::string& descriptor,
                                  const ConstPool& pool);
    void linkInnerClasses(bc_module::BcClass& cls,
                          const InnerClassesAttr& inner,
                          const ConstPool& pool);
};

} // namespace jvm_parser
} // namespace retdec

#endif // RETDEC_JVM_PARSER_JVM_JAR_READER_H
