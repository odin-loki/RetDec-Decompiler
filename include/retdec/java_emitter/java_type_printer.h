/**
 * @file include/retdec/java_emitter/java_type_printer.h
 * @brief BcType → Java source type name with import tracking.
 *
 * Converts BcType values to the syntactically correct Java source form:
 *
 *   BcPrimType::Int                   → "int"
 *   BcPrimType::Long                  → "long"
 *   BcRefType{Class,"java.lang.String"} → "String"  (java.lang auto-imported)
 *   BcRefType{Array,element=Int}      → "int[]"
 *   BcRefType{Generic,"List",args=[String]} → "List<String>"
 *
 * Every non-java.lang reference type is registered in an `ImportSet` so the
 * file emitter can emit the correct import statements at the top of the file.
 *
 * ## Import rules (mirrors javac)
 *
 * - java.lang.* is never emitted as an import.
 * - Types in the same package as the emitted class are not imported.
 * - On collision (two types with the same simple name from different packages),
 *   the first one encountered keeps the short name; subsequent ones are emitted
 *   fully-qualified.
 * - Array types import their element type.
 * - Generic type arguments are recursively imported.
 */

#ifndef RETDEC_JAVA_EMITTER_JAVA_TYPE_PRINTER_H
#define RETDEC_JAVA_EMITTER_JAVA_TYPE_PRINTER_H

#include "retdec/bc_module/bc_type.h"

#include <map>
#include <set>
#include <string>
#include <vector>

namespace retdec {
namespace java_emitter {

using namespace bc_module;

// ─── Import set ───────────────────────────────────────────────────────────────

/**
 * @brief Tracks all types that need import statements.
 *
 * Call `require()` for every type referenced in the emitted file.
 * Call `importLines()` to get the sorted list of `import foo.bar.Baz;` lines.
 */
class ImportSet {
public:
    explicit ImportSet(const std::string& currentPackage = "",
                       const std::string& currentClass = "");

    /**
     * @brief Register a fully-qualified class name for import.
     *
     * Returns the simple name to use in source (or the FQ name on collision).
     */
    std::string require(const std::string& fqName);

    /**
     * @brief Return sorted import lines ready to emit.
     *
     * Each element is a complete line: "import java.util.List;"
     */
    std::vector<std::string> importLines() const;

    /**
     * @brief True if `simpleName` is already mapped to a different FQ name.
     */
    bool hasConflict(const std::string& simpleName,
                     const std::string& fqName) const;

private:
    std::string currentPackage_;
    std::string currentClass_;

    // simple name → first FQ name that claimed it
    std::map<std::string, std::string> simpleTofq_;
    // ordered set of FQ names to import
    std::set<std::string> toImport_;

    static bool isJavaLang(const std::string& fqName);
    static std::string simpleName(const std::string& fqName);
    static std::string packageOf(const std::string& fqName);
};

// ─── Type printer ─────────────────────────────────────────────────────────────

/**
 * @brief Converts BcType values to Java source type strings.
 *
 * All reference types encountered are registered with the `ImportSet`
 * so import statements can be emitted later.
 */
class JavaTypePrinter {
public:
    explicit JavaTypePrinter(ImportSet& imports);

    /**
     * @brief Convert a BcType to its Java source representation.
     *
     * Examples:
     *   Int           → "int"
     *   Long          → "long"
     *   Bool          → "boolean"
     *   Byte          → "byte"
     *   Short         → "short"
     *   Char          → "char"
     *   Float         → "float"
     *   Double        → "double"
     *   Void          → "void"
     *   Class(String) → "String"   (registered in imports)
     *   Array(Int)    → "int[]"
     *   Array(Object) → "Object[]" (registered in imports)
     *   Generic(List,[String]) → "List<String>"
     *   TypeVar(T)    → "T"
     *   Wildcard(+,Comparable) → "? extends Comparable"
     *   Wildcard(-,Object)     → "? super Object"
     *   Wildcard(none)         → "?"
     *   Null                   → "null"  (only in typeof context)
     */
    std::string print(const BcType& type) const;

    /**
     * @brief Print a BcFuncType as a return type + comma-separated params.
     *
     * Returns the return type string; populates `outParams` with param strings.
     */
    std::string printMethod(const BcFuncType& func,
                             std::vector<std::string>& outParams) const;

    /**
     * @brief Print a type without registering imports (for diagnostic use).
     */
    std::string printNoImport(const BcType& type) const;

private:
    ImportSet& imports_;

    std::string printPrim(BcPrimType prim) const;
    std::string printRef(const BcRefType& ref, bool doImport) const;
    std::string printImpl(const BcType& type, bool doImport) const;
};

} // namespace java_emitter
} // namespace retdec

#endif // RETDEC_JAVA_EMITTER_JAVA_TYPE_PRINTER_H
