/**
 * @file include/retdec/cli_parser/cli_reader.h
 * @brief Top-level .NET CLI reader — produces a BcModule from a PE assembly.
 *
 * ## Pipeline
 *
 *   PeReader         → locate streams, resolve RVAs
 *   CliHeaps         → decode #Strings, #US, #GUID, #Blob
 *   MetadataTables   → decode #~ stream (all 38 populated tables)
 *   CliSigDecoder    → decode type/method/local signatures from #Blob
 *   CILLifter        → decode method bodies → BcCFG
 *   CLIReader        → assemble BcModule (classes, fields, methods)
 *
 * ## BcModule population
 *
 * For each TypeDef row (skipping the special <Module> at index 1):
 *   1. Create a BcClass with the correct FQ name, access flags, hierarchy.
 *   2. For each Field in the type's field range:
 *      - Decode the FieldSig → BcType.
 *      - Create a BcField.
 *   3. For each MethodDef in the type's method range:
 *      - Decode the MethodDefSig → BcFuncType.
 *      - Decode parameter names from the Param table.
 *      - If RVA ≠ 0: decode the CIL body → BcCFG.
 *      - Create a BcMethod.
 *   4. Collect GenericParam rows for the class and its methods.
 *   5. Collect CustomAttribute rows → BcAnnotation.
 *   6. Collect InterfaceImpl → BcClass::interfaces.
 *   7. Set BcClass::superClass from TypeDef.extends.
 *
 * ## Portable PDB (optional)
 *
 * If a matching .pdb file is present, sequence points are decoded and
 * stored in BcMethod::locals (mapped to LVT-like data).
 * This is attempted but never fatal.
 *
 * ## Type name format
 *
 * Output uses CLR dot-notation: "System.String", "System.Collections.Generic.List`1"
 * Nested classes: "Outer+Inner"
 * Generic parameters: "`0", "`1", … (positional)
 */

#ifndef RETDEC_CLI_PARSER_CLI_READER_H
#define RETDEC_CLI_PARSER_CLI_READER_H

#include "retdec/bc_module/bc_module.h"
#include "retdec/cli_parser/cil_lifter.h"
#include "retdec/cli_parser/cli_heaps.h"
#include "retdec/cli_parser/cli_sig.h"
#include "retdec/cli_parser/cli_tables.h"
#include "retdec/cli_parser/pe_reader.h"

#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace retdec {
namespace cli_parser {

using namespace bc_module;

// ─── Options ─────────────────────────────────────────────────────────────────

struct CliReadOptions {
    bool decodeCIL          = true;   ///< Decode method bodies
    bool decodeDebugInfo    = true;   ///< Load portable PDB if present
    bool skipSynthetic      = true;   ///< Skip compiler-generated types
    bool skipPrivate        = false;  ///< Include private types
    bool decodeCustomAttrs  = true;   ///< Decode custom attributes → BcAnnotation
    bool decodeGenericParams= true;   ///< Decode generic parameters
    bool decodeNestedClasses= true;
    std::string pdbPath;              ///< Override PDB search path
};

// ─── Parse result ─────────────────────────────────────────────────────────────

struct CliReadResult {
    BcModule    module;
    bool        success = false;
    std::string error;

    // Statistics
    uint32_t typeDefCount    = 0;
    uint32_t methodDefCount  = 0;
    uint32_t fieldCount      = 0;
    uint32_t parseErrorCount = 0;
};

// ─── CLI reader ───────────────────────────────────────────────────────────────

/**
 * @brief Parses a .NET PE assembly and produces a BcModule.
 *
 * Also implements ITypeNameResolver so that CliSigDecoder can resolve
 * TypeDef/TypeRef tokens back to names during signature decoding.
 */
class CLIReader : public ITypeNameResolver {
public:
    explicit CLIReader(const CliReadOptions& opts = CliReadOptions{});

    /**
     * @brief Parse a .NET assembly from a memory buffer.
     *
     * @param data   Raw PE bytes.
     * @param size   Size of the buffer.
     * @param name   Assembly name (used as BcModule name).
     */
    CliReadResult read(const uint8_t* data, size_t size,
                       const std::string& name = "");

    /**
     * @brief Parse a .NET assembly from a file path.
     */
    CliReadResult readFile(const std::string& path);

    // ── ITypeNameResolver ─────────────────────────────────────────────────

    std::string typeDefName(uint32_t idx) const override;
    std::string typeRefName(uint32_t idx) const override;
    BcType      typeSpecType(uint32_t idx) const override;

private:
    CliReadOptions opts_;

    // State during a single read() call.
    const uint8_t*           fileData_  = nullptr;
    size_t                   fileSize_  = 0;
    PeReader                 pe_;
    std::unique_ptr<CliHeaps>       heaps_;
    std::unique_ptr<MetadataTables> tables_;
    std::unique_ptr<CliSigDecoder>  sigDecoder_;
    std::unique_ptr<CILLifter>      cilLifter_;

    // Cache: TypeDef index → resolved FQ name
    mutable std::vector<std::string> typeDefNames_;
    mutable std::vector<std::string> typeRefNames_;

    // ── Build phases ─────────────────────────────────────────────────────

    bool buildTypeNames();
    BcClass buildClass(uint32_t typeDefIdx, CliReadResult& result) const;
    BcField buildField(uint32_t fieldIdx) const;
    BcMethod buildMethod(uint32_t methodDefIdx, CliReadResult& result) const;

    // ── Helper: resolve hierarchy ─────────────────────────────────────────

    std::optional<BcType> resolveExtends(MetadataToken tok) const;
    std::vector<BcType>   resolveInterfaces(uint32_t typeDefIdx) const;

    // ── Helper: generic parameters ────────────────────────────────────────

    std::vector<std::string> genericParams(uint32_t owner,
                                            bool isMethod) const;

    // ── Helper: custom attributes → BcAnnotation ──────────────────────────

    std::vector<BcAnnotation> customAttributes(MetadataToken parent) const;

    // ── Helper: access flags → BcAccess ───────────────────────────────────

    static BcAccess typeDefAccess(uint32_t flags);
    static BcAccess methodDefAccess(uint16_t flags);
    static BcAccess fieldAccess(uint16_t flags);

    // ── Helper: type FQ name construction ────────────────────────────────

    std::string buildFQName(uint32_t ns, uint32_t name) const;
    std::string makeClrName(const std::string& ns, const std::string& name) const;

    // ── Helper: nested class map ──────────────────────────────────────────

    // Built once: nestedClass → enclosingClass (both 1-based TypeDef indices)
    std::vector<std::pair<uint32_t, uint32_t>> nestedMap_;

    bool isNested(uint32_t typeDefIdx) const;
    uint32_t enclosingClass(uint32_t typeDefIdx) const;

    // ── Constant value decoding ───────────────────────────────────────────

    std::optional<int64_t>     fieldConstantInt(uint32_t fieldIdx) const;
    std::optional<double>      fieldConstantFloat(uint32_t fieldIdx) const;
    std::optional<std::string> fieldConstantString(uint32_t fieldIdx) const;
};

} // namespace cli_parser
} // namespace retdec

#endif // RETDEC_CLI_PARSER_CLI_READER_H
