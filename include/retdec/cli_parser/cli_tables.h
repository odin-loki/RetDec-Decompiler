/**
 * @file include/retdec/cli_parser/cli_tables.h
 * @brief ECMA-335 metadata table decoder.
 *
 * ## Metadata tables (§II.22)
 *
 * The #~ (or #-) stream contains a sequence of metadata tables.  Each table
 * is an array of fixed-width rows.  Column widths depend on:
 *   - HeapSizes flags (for #Strings, #Blob, #GUID indices)
 *   - Row counts in other tables (coded token widths)
 *
 * ## Coded tokens (§II.24.2.6)
 *
 * Certain columns encode a table index AND which table via a small tag:
 *
 *   TypeDefOrRef:   2 bits tag — TypeDef(0) TypeRef(1) TypeSpec(2)
 *   HasConstant:    2 bits tag — Field(0) Param(1) Property(2)
 *   HasCustomAttribute: 5 bits — ...16 possible tables...
 *   HasFieldMarshal:    1 bit  — Field(0) Param(1)
 *   HasDeclSecurity:    2 bits — TypeDef(0) MethodDef(1) Assembly(2)
 *   MemberRefParent:    3 bits — TypeDef(0) TypeRef(1) ModuleRef(2) MethodDef(3) TypeSpec(4)
 *   HasSemantics:       1 bit  — Event(0) Property(1)
 *   MethodDefOrRef:     1 bit  — MethodDef(0) MemberRef(1)
 *   MemberForwarded:    1 bit  — Field(0) MethodDef(1)
 *   Implementation:     2 bits — File(0) AssemblyRef(1) ExportedType(2)
 *   CustomAttributeType:3 bits — MethodDef(2) MemberRef(3)
 *   ResolutionScope:    2 bits — Module(0) ModuleRef(1) AssemblyRef(2) TypeRef(3)
 *   TypeOrMethodDef:    1 bit  — TypeDef(0) MethodDef(1)
 *
 * ## Table IDs (§II.22.1)
 *
 * 0x00 Module              0x01 TypeRef
 * 0x02 TypeDef             0x04 Field
 * 0x06 MethodDef           0x08 Param
 * 0x09 InterfaceImpl       0x0A MemberRef
 * 0x0B Constant            0x0C CustomAttribute
 * 0x0D FieldMarshal        0x0E DeclSecurity
 * 0x0F ClassLayout         0x10 FieldLayout
 * 0x11 StandAloneSig       0x12 EventMap
 * 0x14 Event               0x15 PropertyMap
 * 0x17 Property            0x18 MethodSemantics
 * 0x19 MethodImpl          0x1A ModuleRef
 * 0x1B TypeSpec            0x1C ImplMap
 * 0x1D FieldRVA            0x20 Assembly
 * 0x21 AssemblyProcessor   0x22 AssemblyOS
 * 0x23 AssemblyRef         0x24 AssemblyRefProcessor
 * 0x25 AssemblyRefOS       0x26 File
 * 0x27 ExportedType        0x28 ManifestResource
 * 0x29 NestedClass         0x2A GenericParam
 * 0x2B MethodSpec          0x2C GenericParamConstraint
 */

#ifndef RETDEC_CLI_PARSER_CLI_TABLES_H
#define RETDEC_CLI_PARSER_CLI_TABLES_H

#include "retdec/cli_parser/cli_heaps.h"

#include <array>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace retdec {
namespace cli_parser {

// ─── Table IDs ────────────────────────────────────────────────────────────────

enum class TableId : uint8_t {
    Module              = 0x00,
    TypeRef             = 0x01,
    TypeDef             = 0x02,
    Field               = 0x04,
    MethodDef           = 0x06,
    Param               = 0x08,
    InterfaceImpl       = 0x09,
    MemberRef           = 0x0A,
    Constant            = 0x0B,
    CustomAttribute     = 0x0C,
    FieldMarshal        = 0x0D,
    DeclSecurity        = 0x0E,
    ClassLayout         = 0x0F,
    FieldLayout         = 0x10,
    StandAloneSig       = 0x11,
    EventMap            = 0x12,
    Event               = 0x14,
    PropertyMap         = 0x15,
    Property            = 0x17,
    MethodSemantics     = 0x18,
    MethodImpl          = 0x19,
    ModuleRef           = 0x1A,
    TypeSpec            = 0x1B,
    ImplMap             = 0x1C,
    FieldRVA            = 0x1D,
    Assembly            = 0x20,
    AssemblyProcessor   = 0x21,
    AssemblyOS          = 0x22,
    AssemblyRef         = 0x23,
    AssemblyRefProcessor= 0x24,
    AssemblyRefOS       = 0x25,
    File                = 0x26,
    ExportedType        = 0x27,
    ManifestResource    = 0x28,
    NestedClass         = 0x29,
    GenericParam        = 0x2A,
    MethodSpec          = 0x2B,
    GenericParamConstraint = 0x2C,
    _Count              = 0x2D,
};

// ─── Metadata token ───────────────────────────────────────────────────────────

struct MetadataToken {
    uint8_t  table = 0;    ///< Table ID (0x00–0x2C)
    uint32_t index = 0;    ///< 1-based row index within the table

    bool valid() const { return index != 0; }
    uint32_t raw() const { return (static_cast<uint32_t>(table) << 24) | index; }
    static MetadataToken fromRaw(uint32_t tok) {
        return {static_cast<uint8_t>(tok >> 24), tok & 0x00FFFFFF};
    }
};

// ─── Row types for each metadata table ───────────────────────────────────────

struct ModuleRow {
    uint16_t generation = 0;
    uint32_t name = 0;        ///< #Strings offset
    uint32_t mvId = 0;        ///< #GUID index
    uint32_t encId = 0;
    uint32_t encBaseId = 0;
};

struct TypeRefRow {
    MetadataToken resolutionScope;  ///< Module | ModuleRef | AssemblyRef | TypeRef
    uint32_t      name = 0;         ///< #Strings
    uint32_t      ns   = 0;         ///< #Strings (namespace)
};

struct TypeDefRow {
    uint32_t      flags = 0;        ///< TypeAttributes
    uint32_t      name  = 0;        ///< #Strings
    uint32_t      ns    = 0;        ///< #Strings
    MetadataToken extends;          ///< TypeDefOrRef
    uint32_t      fieldList   = 0;  ///< 1-based start into Field table
    uint32_t      methodList  = 0;  ///< 1-based start into MethodDef table
};

struct FieldRow {
    uint16_t flags      = 0;  ///< FieldAttributes
    uint32_t name       = 0;  ///< #Strings
    uint32_t signature  = 0;  ///< #Blob: FieldSig
};

struct MethodDefRow {
    uint32_t rva          = 0;  ///< RVA of CIL method body
    uint16_t implFlags    = 0;  ///< MethodImplAttributes
    uint16_t flags        = 0;  ///< MethodAttributes
    uint32_t name         = 0;  ///< #Strings
    uint32_t signature    = 0;  ///< #Blob: MethodDefSig
    uint32_t paramList    = 0;  ///< 1-based start into Param table
};

struct ParamRow {
    uint16_t flags    = 0;
    uint16_t sequence = 0;  ///< 0 = return type, 1..N = parameters
    uint32_t name     = 0;  ///< #Strings
};

struct InterfaceImplRow {
    uint32_t      clazz     = 0;  ///< 1-based TypeDef index
    MetadataToken interface_;
};

struct MemberRefRow {
    MetadataToken clazz;    ///< MemberRefParent coded token
    uint32_t      name      = 0;
    uint32_t      signature = 0;  ///< #Blob
};

struct ConstantRow {
    uint8_t       type   = 0;   ///< ELEMENT_TYPE_*
    MetadataToken parent;        ///< HasConstant coded token
    uint32_t      value  = 0;   ///< #Blob
};

struct CustomAttributeRow {
    MetadataToken parent;   ///< HasCustomAttribute
    MetadataToken type;     ///< CustomAttributeType
    uint32_t      value = 0; ///< #Blob
};

struct ClassLayoutRow {
    uint16_t packingSize = 0;
    uint32_t classSize   = 0;
    uint32_t parent      = 0;  ///< TypeDef index
};

struct StandAloneSigRow {
    uint32_t signature = 0;  ///< #Blob
};

struct PropertyRow {
    uint16_t flags     = 0;
    uint32_t name      = 0;
    uint32_t type      = 0;  ///< #Blob: PropertySig
};

struct MethodSemanticsRow {
    uint16_t      semantics = 0;  ///< Getter=0x0002, Setter=0x0001, Other=0x0004
    uint32_t      method    = 0;  ///< MethodDef index
    MetadataToken association;    ///< HasSemantics coded
};

struct MethodImplRow {
    uint32_t      clazz           = 0;
    MetadataToken methodBody;      ///< MethodDefOrRef
    MetadataToken methodDeclaration;
};

struct ModuleRefRow {
    uint32_t name = 0;
};

struct TypeSpecRow {
    uint32_t signature = 0;  ///< #Blob: TypeSpec
};

struct ImplMapRow {
    uint16_t      mappingFlags  = 0;
    MetadataToken memberForwarded;
    uint32_t      importName    = 0;
    uint32_t      importScope   = 0;  ///< ModuleRef index
};

struct FieldRVARow {
    uint32_t rva   = 0;
    uint32_t field = 0;  ///< Field index
};

struct AssemblyRow {
    uint32_t hashAlgId         = 0;
    uint16_t majorVersion      = 0;
    uint16_t minorVersion      = 0;
    uint16_t buildNumber       = 0;
    uint16_t revisionNumber    = 0;
    uint32_t flags             = 0;
    uint32_t publicKey         = 0;  ///< #Blob
    uint32_t name              = 0;  ///< #Strings
    uint32_t culture           = 0;  ///< #Strings
};

struct AssemblyRefRow {
    uint16_t majorVersion      = 0;
    uint16_t minorVersion      = 0;
    uint16_t buildNumber       = 0;
    uint16_t revisionNumber    = 0;
    uint32_t flags             = 0;
    uint32_t publicKeyOrToken  = 0;  ///< #Blob
    uint32_t name              = 0;  ///< #Strings
    uint32_t culture           = 0;  ///< #Strings
    uint32_t hashValue         = 0;  ///< #Blob
};

struct NestedClassRow {
    uint32_t nestedClass    = 0;  ///< TypeDef index
    uint32_t enclosingClass = 0;  ///< TypeDef index
};

struct GenericParamRow {
    uint16_t      number    = 0;  ///< Zero-based parameter index
    uint16_t      flags     = 0;  ///< GenericParamAttributes
    MetadataToken owner;          ///< TypeOrMethodDef coded
    uint32_t      name      = 0;  ///< #Strings
};

struct MethodSpecRow {
    MetadataToken method;         ///< MethodDefOrRef
    uint32_t      instantiation = 0; ///< #Blob: MethodSpec signature
};

struct GenericParamConstraintRow {
    uint32_t      owner      = 0;  ///< GenericParam index
    MetadataToken constraint;      ///< TypeDefOrRef
};

struct EventRow {
    uint16_t      flags     = 0;
    uint32_t      name      = 0;
    MetadataToken eventType;   ///< TypeDefOrRef
};

struct PropertyMapRow {
    uint32_t parent       = 0;  ///< TypeDef index
    uint32_t propertyList = 0;  ///< Property index
};

struct EventMapRow {
    uint32_t parent    = 0;  ///< TypeDef index
    uint32_t eventList = 0;  ///< Event index
};

struct FileRow {
    uint32_t flags     = 0;
    uint32_t name      = 0;
    uint32_t hashValue = 0;  ///< #Blob
};

struct ManifestResourceRow {
    uint32_t      offset         = 0;
    uint32_t      flags          = 0;
    uint32_t      name           = 0;
    MetadataToken implementation;
};

struct ExportedTypeRow {
    uint32_t      flags          = 0;
    uint32_t      typeDefId      = 0;
    uint32_t      typeName       = 0;
    uint32_t      typeNamespace  = 0;
    MetadataToken implementation;
};

// ─── TypeDef flags ────────────────────────────────────────────────────────────

namespace TypeAttributes {
    static constexpr uint32_t VisibilityMask      = 0x00000007;
    static constexpr uint32_t NotPublic           = 0x00000000;
    static constexpr uint32_t Public              = 0x00000001;
    static constexpr uint32_t NestedPublic        = 0x00000002;
    static constexpr uint32_t NestedPrivate       = 0x00000003;
    static constexpr uint32_t NestedFamily        = 0x00000004;
    static constexpr uint32_t NestedAssembly      = 0x00000005;
    static constexpr uint32_t NestedFamANDAssem   = 0x00000006;
    static constexpr uint32_t NestedFamORAssem    = 0x00000007;
    static constexpr uint32_t LayoutMask          = 0x00000018;
    static constexpr uint32_t AutoLayout          = 0x00000000;
    static constexpr uint32_t SequentialLayout    = 0x00000008;
    static constexpr uint32_t ExplicitLayout      = 0x00000010;
    static constexpr uint32_t ClassSemanticsMask  = 0x00000020;
    static constexpr uint32_t Class               = 0x00000000;
    static constexpr uint32_t Interface           = 0x00000020;
    static constexpr uint32_t Abstract            = 0x00000080;
    static constexpr uint32_t Sealed              = 0x00000100;
    static constexpr uint32_t SpecialName         = 0x00000400;
    static constexpr uint32_t Import              = 0x00001000;  // COM import
    static constexpr uint32_t Serializable        = 0x00002000;
    static constexpr uint32_t WindowsRuntime      = 0x00004000;
    static constexpr uint32_t StringFormatMask    = 0x00030000;
    static constexpr uint32_t BeforeFieldInit     = 0x00100000;
}

// ─── MethodDef flags ──────────────────────────────────────────────────────────

namespace MethodAttributes {
    static constexpr uint16_t MemberAccessMask    = 0x0007;
    static constexpr uint16_t CompilerControlled  = 0x0000;
    static constexpr uint16_t Private             = 0x0001;
    static constexpr uint16_t FamANDAssem         = 0x0002;
    static constexpr uint16_t Assem               = 0x0003;
    static constexpr uint16_t Family              = 0x0004;
    static constexpr uint16_t FamORAssem          = 0x0005;
    static constexpr uint16_t Public              = 0x0006;
    static constexpr uint16_t Static              = 0x0010;
    static constexpr uint16_t Final               = 0x0020;
    static constexpr uint16_t Virtual             = 0x0040;
    static constexpr uint16_t HideBySig           = 0x0080;
    static constexpr uint16_t NewSlot             = 0x0100;
    static constexpr uint16_t CheckAccessOnOverride= 0x0200;
    static constexpr uint16_t Abstract            = 0x0400;
    static constexpr uint16_t SpecialName         = 0x0800;
    static constexpr uint16_t PInvokeImpl         = 0x2000;
    static constexpr uint16_t RTSpecialName       = 0x1000;
}

// ─── MetadataTable — raw row storage and typed row access ────────────────────

/**
 * @brief Holds row data for a single metadata table.
 *
 * Rows are stored as parsed structs in a type-erased vector.
 * Typed access is via the strongly-typed getter methods on MetadataTables.
 */
struct RawTable {
    uint32_t rowCount = 0;
    uint32_t rowSize  = 0;
    std::vector<uint8_t> data; ///< rowCount × rowSize bytes
};

// ─── MetadataTables ───────────────────────────────────────────────────────────

/**
 * @brief Decodes and stores all 45 ECMA-335 metadata tables.
 *
 * Parsing is done in a single pass over the #~ stream.
 * Row counts are determined first, then row widths are computed (they
 * depend on heap sizes and row counts of referenced tables), and finally
 * each table's rows are read.
 */
class MetadataTables {
public:
    MetadataTables() = default;

    /**
     * @brief Parse the #~ (or #-) stream.
     *
     * @param tilde   Byte span of the entire #~ stream.
     * @param heaps   Heap set for index width determination.
     * @return true on success.
     */
    bool parse(std::span<const uint8_t> tilde, const CliHeaps& heaps);

    bool isValid() const { return valid_; }
    const std::string& error() const { return error_; }

    /// Number of rows in a table (0 if table is absent).
    uint32_t rowCount(TableId id) const;

    // ── Typed row accessors ────────────────────────────────────────────────

    ModuleRow              module(uint32_t idx) const;          ///< 1-based
    TypeRefRow             typeRef(uint32_t idx) const;
    TypeDefRow             typeDef(uint32_t idx) const;
    FieldRow               field(uint32_t idx) const;
    MethodDefRow           methodDef(uint32_t idx) const;
    ParamRow               param(uint32_t idx) const;
    InterfaceImplRow       interfaceImpl(uint32_t idx) const;
    MemberRefRow           memberRef(uint32_t idx) const;
    ConstantRow            constant(uint32_t idx) const;
    CustomAttributeRow     customAttribute(uint32_t idx) const;
    ClassLayoutRow         classLayout(uint32_t idx) const;
    StandAloneSigRow       standAloneSig(uint32_t idx) const;
    PropertyRow            property(uint32_t idx) const;
    MethodSemanticsRow     methodSemantics(uint32_t idx) const;
    MethodImplRow          methodImpl(uint32_t idx) const;
    ModuleRefRow           moduleRef(uint32_t idx) const;
    TypeSpecRow            typeSpec(uint32_t idx) const;
    ImplMapRow             implMap(uint32_t idx) const;
    FieldRVARow            fieldRVA(uint32_t idx) const;
    AssemblyRow            assembly(uint32_t idx) const;
    AssemblyRefRow         assemblyRef(uint32_t idx) const;
    NestedClassRow         nestedClass(uint32_t idx) const;
    GenericParamRow        genericParam(uint32_t idx) const;
    MethodSpecRow          methodSpec(uint32_t idx) const;
    GenericParamConstraintRow genericParamConstraint(uint32_t idx) const;
    EventRow               event(uint32_t idx) const;
    PropertyMapRow         propertyMap(uint32_t idx) const;
    EventMapRow            eventMap(uint32_t idx) const;
    FileRow                file(uint32_t idx) const;
    ManifestResourceRow    manifestResource(uint32_t idx) const;
    ExportedTypeRow        exportedType(uint32_t idx) const;

    /// Resolve a coded token to a MetadataToken for TypeDefOrRef.
    MetadataToken decodeTypeDefOrRef(uint32_t coded) const;
    /// Resolve a coded token for ResolutionScope.
    MetadataToken decodeResolutionScope(uint32_t coded) const;
    /// Resolve a coded token for MemberRefParent.
    MetadataToken decodeMemberRefParent(uint32_t coded) const;
    /// Resolve a coded token for HasCustomAttribute.
    MetadataToken decodeHasCustomAttribute(uint32_t coded) const;
    /// Resolve a coded token for CustomAttributeType.
    MetadataToken decodeCustomAttributeType(uint32_t coded) const;
    /// Resolve a coded token for TypeOrMethodDef.
    MetadataToken decodeTypeOrMethodDef(uint32_t coded) const;
    /// Resolve a coded token for MethodDefOrRef.
    MetadataToken decodeMethodDefOrRef(uint32_t coded) const;
    /// Resolve a coded token for HasSemantics.
    MetadataToken decodeHasSemantics(uint32_t coded) const;
    /// Resolve a coded token for MemberForwarded.
    MetadataToken decodeMemberForwarded(uint32_t coded) const;
    /// Resolve a coded token for Implementation.
    MetadataToken decodeImplementation(uint32_t coded) const;
    /// Resolve a coded token for HasConstant.
    MetadataToken decodeHasConstant(uint32_t coded) const;

private:
    static constexpr size_t kMaxTables = static_cast<size_t>(TableId::_Count);

    bool        valid_ = false;
    std::string error_;

    std::array<RawTable, kMaxTables> tables_;

    // Width information computed during parse
    bool wideStrings_ = false;
    bool wideGuid_    = false;
    bool wideBlob_    = false;

    // Row counts for coded-token width decisions
    uint32_t rowCount_[kMaxTables] = {};

    // Coding helpers
    bool codedTokenWide(const uint8_t* tables, size_t count, uint8_t tagBits) const;

    // Row parsing: returns the typed struct from raw bytes at position pos.
    struct RowReader {
        const uint8_t*          data;
        size_t                  pos;
        bool wideStr, wideGuid, wideBlob;
        const uint32_t*         rowCounts;

        uint8_t  u8();
        uint16_t u16();
        uint32_t u32();
        uint32_t strIdx();    ///< 2 or 4 bytes
        uint32_t guidIdx();
        uint32_t blobIdx();
        uint32_t tableIdx(TableId tbl);  ///< 2 or 4 bytes depending on table size
        uint32_t codedIdx(const uint8_t* tableIds, size_t count, uint8_t tagBits);
        MetadataToken codedToken(const uint8_t* tableIds, size_t count, uint8_t tagBits);
    };

    bool parseTable(TableId id, RowReader& rr);
    size_t computeRowSize(TableId id) const;
    void computeWidths(const CliHeaps& heaps);
};

} // namespace cli_parser
} // namespace retdec

#endif // RETDEC_CLI_PARSER_CLI_TABLES_H
