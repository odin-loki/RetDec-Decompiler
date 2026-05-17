/**
 * @file include/retdec/wasm_parser/wasm_types.h
 * @brief WebAssembly binary format type system and fundamental structures.
 *
 * Implements the WebAssembly 2.0 core spec (W3C, 2022) + threads,
 * reference-types, SIMD, bulk-memory, and tail-calls proposals.
 *
 * References:
 *   - https://webassembly.github.io/spec/core/binary/types.html
 *   - https://webassembly.github.io/spec/core/binary/modules.html
 */

#ifndef RETDEC_WASM_PARSER_WASM_TYPES_H
#define RETDEC_WASM_PARSER_WASM_TYPES_H

#include <cstdint>
#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace retdec {
namespace wasm_parser {

// ─── Value types ─────────────────────────────────────────────────────────────

enum class ValType : uint8_t {
    // Number types
    I32    = 0x7F,
    I64    = 0x7E,
    F32    = 0x7D,
    F64    = 0x7C,
    // Vector type (SIMD)
    V128   = 0x7B,
    // Reference types
    FuncRef   = 0x70,
    ExternRef = 0x6F,
};

std::string valTypeStr(ValType vt);

// ─── Function type ───────────────────────────────────────────────────────────

struct FuncType {
    std::vector<ValType> params;
    std::vector<ValType> results;

    bool operator==(const FuncType& o) const {
        return params == o.params && results == o.results;
    }
};

// ─── Limits ──────────────────────────────────────────────────────────────────

struct Limits {
    uint32_t min = 0;
    std::optional<uint32_t> max;
    bool shared = false;  ///< threads proposal
};

// ─── Memory / Table types ────────────────────────────────────────────────────

struct MemType {
    Limits limits;
};

struct TableType {
    ValType  refType = ValType::FuncRef;
    Limits   limits;
};

// ─── Global type ─────────────────────────────────────────────────────────────

struct GlobalType {
    ValType valType = ValType::I32;
    bool    isMutable = false;
};

// ─── Sections ─────────────────────────────────────────────────────────────────

enum class SectionId : uint8_t {
    Custom   = 0,
    Type     = 1,
    Import   = 2,
    Function = 3,
    Table    = 4,
    Memory   = 5,
    Global   = 6,
    Export   = 7,
    Start    = 8,
    Element  = 9,
    Code     = 10,
    Data     = 11,
    DataCount= 12,
};

// ─── Import / Export descriptors ─────────────────────────────────────────────

enum class ExternKind : uint8_t {
    Func   = 0,
    Table  = 1,
    Memory = 2,
    Global = 3,
};

struct Import {
    std::string module;
    std::string name;
    ExternKind  kind = ExternKind::Func;
    uint32_t    index = 0;   ///< type index (func) or table/mem/global index
    // For non-func imports:
    std::optional<TableType>  tableType;
    std::optional<MemType>    memType;
    std::optional<GlobalType> globalType;
};

struct Export {
    std::string name;
    ExternKind  kind  = ExternKind::Func;
    uint32_t    index = 0;
};

// ─── Global ──────────────────────────────────────────────────────────────────

struct WasmGlobal {
    GlobalType  type;
    std::vector<uint8_t> initExpr;   ///< constant expression bytecode
};

// ─── Element segment ─────────────────────────────────────────────────────────

struct ElementSegment {
    uint32_t                tableIndex = 0;
    std::vector<uint8_t>    offsetExpr;       ///< constant expr for offset
    std::vector<uint32_t>   funcIndices;      ///< for legacy elem format
    ValType                 refType = ValType::FuncRef;
    // element expressions (funcrefs / externrefs)
    std::vector<std::vector<uint8_t>> elemExprs;
    bool isPassive = false;
    bool isDeclarative = false;
};

// ─── Data segment ────────────────────────────────────────────────────────────

struct DataSegment {
    uint32_t             memIndex = 0;
    std::vector<uint8_t> offsetExpr;
    std::vector<uint8_t> bytes;
    bool                 isPassive = false;
};

// ─── Local variable in a function body ───────────────────────────────────────

struct WasmLocal {
    uint32_t count = 0;
    ValType  type  = ValType::I32;
};

// ─── Function code ───────────────────────────────────────────────────────────

struct FuncCode {
    std::vector<WasmLocal> locals;
    std::vector<uint8_t>   body;      ///< raw expression bytes (incl. END)
};

// ─── Custom section ───────────────────────────────────────────────────────────

struct CustomSection {
    std::string          name;
    std::vector<uint8_t> data;
};

// ─── Name section entries (debug info) ───────────────────────────────────────

struct NameMap {
    uint32_t    index = 0;
    std::string name;
};

struct IndirectNameMap {
    uint32_t              index = 0;
    std::vector<NameMap>  names;
};

struct NameSection {
    std::optional<std::string>          moduleName;
    std::vector<NameMap>                funcNames;
    std::vector<IndirectNameMap>        localNames;
    std::vector<NameMap>                globalNames;
    std::vector<NameMap>                memNames;
    std::vector<NameMap>                tableNames;
    std::vector<NameMap>                typeNames;
};

// ─── WasmModule ──────────────────────────────────────────────────────────────

/**
 * @brief Top-level in-memory representation of a parsed .wasm module.
 */
struct WasmModule {
    // Type section
    std::vector<FuncType>       types;

    // Import section
    std::vector<Import>         imports;

    // Function section (maps func idx → type idx, for non-imported funcs)
    std::vector<uint32_t>       funcTypeIndices;

    // Table section
    std::vector<TableType>      tables;

    // Memory section
    std::vector<MemType>        memories;

    // Global section
    std::vector<WasmGlobal>     globals;

    // Export section
    std::vector<Export>         exports;

    // Start function index (optional)
    std::optional<uint32_t>     startFunc;

    // Element section
    std::vector<ElementSegment> elements;

    // Code section
    std::vector<FuncCode>       codes;

    // Data section
    std::vector<DataSegment>    dataSegments;

    // Custom sections
    std::vector<CustomSection>  customSections;

    // Parsed name section (from "name" custom section)
    std::optional<NameSection>  names;

    // ── Index space helpers ──────────────────────────────────────────────────

    /// Number of imported functions
    uint32_t importedFuncCount() const;

    /// Total function count (imported + defined)
    uint32_t totalFuncCount() const;

    /// Type index for function at absolute index funcIdx
    uint32_t funcTypeIndex(uint32_t funcIdx) const;

    /// Debug name for a function (from name section or generated)
    std::string funcName(uint32_t funcIdx) const;

    /// Debug name for a global
    std::string globalName(uint32_t idx) const;

    /// Export name for a function (if exported)
    std::optional<std::string> funcExportName(uint32_t funcIdx) const;
};

} // namespace wasm_parser
} // namespace retdec

#endif // RETDEC_WASM_PARSER_WASM_TYPES_H
