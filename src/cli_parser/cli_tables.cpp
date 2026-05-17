/**
 * @file src/cli_parser/cli_tables.cpp
 * @brief ECMA-335 metadata table decoder.
 */

#include "retdec/cli_parser/cli_tables.h"

#include <algorithm>
#include <cassert>
#include <cstring>
#include <stdexcept>

namespace retdec {
namespace cli_parser {

// ─── RowReader helpers ────────────────────────────────────────────────────────

uint8_t MetadataTables::RowReader::u8() {
    return data[pos++];
}

uint16_t MetadataTables::RowReader::u16() {
    uint16_t v = static_cast<uint16_t>(data[pos]) |
                 (static_cast<uint16_t>(data[pos + 1]) << 8);
    pos += 2;
    return v;
}

uint32_t MetadataTables::RowReader::u32() {
    uint32_t v = static_cast<uint32_t>(data[pos])        |
                 (static_cast<uint32_t>(data[pos + 1]) << 8)  |
                 (static_cast<uint32_t>(data[pos + 2]) << 16) |
                 (static_cast<uint32_t>(data[pos + 3]) << 24);
    pos += 4;
    return v;
}

uint32_t MetadataTables::RowReader::strIdx() {
    return wideStr ? u32() : u16();
}

uint32_t MetadataTables::RowReader::guidIdx() {
    return wideGuid ? u32() : u16();
}

uint32_t MetadataTables::RowReader::blobIdx() {
    return wideBlob ? u32() : u16();
}

uint32_t MetadataTables::RowReader::tableIdx(TableId tbl) {
    uint32_t id = static_cast<uint32_t>(tbl);
    if (id < static_cast<uint32_t>(TableId::_Count) && rowCounts[id] > 0xFFFF)
        return u32();
    return u16();
}

// Coded token: tagBits low bits = tag, remaining high bits = row index
uint32_t MetadataTables::RowReader::codedIdx(
        const uint8_t* tableIds, size_t count, uint8_t tagBits) {
    // Determine if we need 4 bytes: any referenced table has > 2^(16-tagBits)-1 rows
    uint32_t maxRows = 1u << (16 - tagBits);
    bool wide = false;
    for (size_t i = 0; i < count && !wide; ++i) {
        uint8_t tid = tableIds[i];
        if (tid < static_cast<uint8_t>(TableId::_Count) &&
            rowCounts[tid] >= maxRows)
            wide = true;
    }
    return wide ? u32() : u16();
}

MetadataToken MetadataTables::RowReader::codedToken(
        const uint8_t* tableIds, size_t count, uint8_t tagBits) {
    uint32_t raw = codedIdx(tableIds, count, tagBits);
    uint32_t tag = raw & ((1u << tagBits) - 1);
    uint32_t idx = raw >> tagBits;
    MetadataToken tok;
    tok.index = idx;
    tok.table = (tag < count) ? tableIds[tag] : 0xFF;
    return tok;
}

// ─── Coded token tables ───────────────────────────────────────────────────────

static const uint8_t kTypeDefOrRef[]  = {0x02, 0x01, 0x1B};           // 2 bits
static const uint8_t kHasConstant[]   = {0x04, 0x08, 0x17};           // 2 bits
static const uint8_t kHasCustomAttr[] = {0x06, 0x04, 0x01, 0x02, 0x08, // 5 bits
                                          0x09, 0x0A, 0x00, 0x11, 0x14,
                                          0x17, 0x18, 0x1A, 0x1B, 0x20,
                                          0x23, 0x26, 0x27, 0x28, 0x2A,
                                          0x2B, 0x2C};
static const uint8_t kHasFieldMarshal[] = {0x04, 0x08};               // 1 bit
static const uint8_t kHasDeclSecurity[] = {0x02, 0x06, 0x20};         // 2 bits
static const uint8_t kMemberRefParent[] = {0x02, 0x01, 0x1A, 0x06, 0x1B}; // 3 bits
static const uint8_t kHasSemantics[]    = {0x14, 0x17};               // 1 bit
static const uint8_t kMethodDefOrRef[]  = {0x06, 0x0A};               // 1 bit
static const uint8_t kMemberForwarded[] = {0x04, 0x06};               // 1 bit
static const uint8_t kImplementation[]  = {0x26, 0x23, 0x27};         // 2 bits
static const uint8_t kCustomAttrType[]  = {0xFF, 0xFF, 0x06, 0x0A, 0xFF}; // 3 bits
static const uint8_t kResolutionScope[] = {0x00, 0x1A, 0x23, 0x01};   // 2 bits
static const uint8_t kTypeOrMethodDef[] = {0x02, 0x06};               // 1 bit

// ─── MetadataTables::parse ────────────────────────────────────────────────────

bool MetadataTables::parse(std::span<const uint8_t> tilde, const CliHeaps& heaps) {
    valid_ = false;

    if (tilde.size() < 24) { error_ = "#~ stream too small"; return false; }

    // #~ stream header (§II.24.2.6)
    // DWORD  Reserved   (must be 0)
    // BYTE   MajorVersion
    // BYTE   MinorVersion
    // BYTE   HeapSizes
    // BYTE   Reserved2
    // QWORD  Valid        (bitmask of present tables)
    // QWORD  Sorted       (bitmask of sorted tables)
    // DWORD  Rows[popcount(Valid)]   (row counts for present tables)

    uint8_t heapSizes = tilde[6];
    uint64_t valid    = 0;
    for (int i = 0; i < 8; ++i)
        valid |= static_cast<uint64_t>(tilde[8 + i]) << (i * 8);

    wideStrings_ = (heapSizes & kHeapSizeStrings) != 0;
    wideGuid_    = (heapSizes & kHeapSizeGUID)    != 0;
    wideBlob_    = (heapSizes & kHeapSizeBlob)    != 0;

    // Row counts
    size_t rowPos = 24;
    std::memset(rowCount_, 0, sizeof(rowCount_));
    for (int t = 0; t < 64 && t < static_cast<int>(TableId::_Count); ++t) {
        if (valid & (1ULL << t)) {
            if (rowPos + 4 > tilde.size()) {
                error_ = "#~ row count truncated";
                return false;
            }
            uint32_t rc = static_cast<uint32_t>(tilde[rowPos])        |
                          (static_cast<uint32_t>(tilde[rowPos + 1]) << 8)  |
                          (static_cast<uint32_t>(tilde[rowPos + 2]) << 16) |
                          (static_cast<uint32_t>(tilde[rowPos + 3]) << 24);
            rowCount_[t] = rc;
            rowPos += 4;
        }
    }

    // Copy to tables_ row counts
    for (int t = 0; t < static_cast<int>(TableId::_Count); ++t)
        tables_[t].rowCount = rowCount_[t];

    // Now parse each table's rows
    RowReader rr;
    rr.data      = tilde.data() + rowPos;
    rr.pos       = 0;
    rr.wideStr   = wideStrings_;
    rr.wideGuid  = wideGuid_;
    rr.wideBlob  = wideBlob_;
    rr.rowCounts = rowCount_;

    for (int t = 0; t < static_cast<int>(TableId::_Count); ++t) {
        if (rowCount_[t] == 0) continue;
        if (!parseTable(static_cast<TableId>(t), rr)) return false;
    }

    valid_ = true;
    return true;
}

uint32_t MetadataTables::rowCount(TableId id) const {
    size_t idx = static_cast<size_t>(id);
    if (idx >= kMaxTables) return 0;
    return tables_[idx].rowCount;
}

// ─── parseTable ──────────────────────────────────────────────────────────────

bool MetadataTables::parseTable(TableId id, RowReader& rr) {
    auto& tbl = tables_[static_cast<size_t>(id)];
    uint32_t n = tbl.rowCount;
    if (n == 0) return true;

    // We store rows in a generic byte buffer, then access them via typed methods.
    // For simplicity, store each row as a vector of up to 10 uint32_t fields.
    // This avoids per-table struct sizing complexity in the parse loop.
    // Each row is stored as: [field0, field1, …, fieldN-1] as uint32_t values.

    static constexpr size_t kMaxFields = 12;
    tbl.rowSize  = kMaxFields * sizeof(uint32_t);
    tbl.data.resize(n * tbl.rowSize, 0);

    for (uint32_t row = 0; row < n; ++row) {
        uint32_t* fields = reinterpret_cast<uint32_t*>(
            tbl.data.data() + row * tbl.rowSize);
        size_t f = 0;

        // Field layout per table:
        switch (id) {
        case TableId::Module:
            fields[f++] = rr.u16();         // Generation
            fields[f++] = rr.strIdx();      // Name
            fields[f++] = rr.guidIdx();     // MvId
            fields[f++] = rr.guidIdx();     // EncId
            fields[f++] = rr.guidIdx();     // EncBaseId
            break;
        case TableId::TypeRef: {
            static const uint8_t tbl2[] = {0x00, 0x1A, 0x23, 0x01};
            auto tok = rr.codedToken(tbl2, 4, 2);
            fields[f++] = tok.table; fields[f++] = tok.index; // ResolutionScope
            fields[f++] = rr.strIdx(); // Name
            fields[f++] = rr.strIdx(); // Namespace
            break;
        }
        case TableId::TypeDef: {
            fields[f++] = rr.u32();         // Flags
            fields[f++] = rr.strIdx();      // Name
            fields[f++] = rr.strIdx();      // Namespace
            auto tok = rr.codedToken(kTypeDefOrRef, 3, 2);
            fields[f++] = tok.table; fields[f++] = tok.index; // Extends
            fields[f++] = rr.tableIdx(TableId::Field);      // FieldList
            fields[f++] = rr.tableIdx(TableId::MethodDef);  // MethodList
            break;
        }
        case TableId::Field:
            fields[f++] = rr.u16();         // Flags
            fields[f++] = rr.strIdx();      // Name
            fields[f++] = rr.blobIdx();     // Signature
            break;
        case TableId::MethodDef:
            fields[f++] = rr.u32();         // RVA
            fields[f++] = rr.u16();         // ImplFlags
            fields[f++] = rr.u16();         // Flags
            fields[f++] = rr.strIdx();      // Name
            fields[f++] = rr.blobIdx();     // Signature
            fields[f++] = rr.tableIdx(TableId::Param); // ParamList
            break;
        case TableId::Param:
            fields[f++] = rr.u16();         // Flags
            fields[f++] = rr.u16();         // Sequence
            fields[f++] = rr.strIdx();      // Name
            break;
        case TableId::InterfaceImpl:
            fields[f++] = rr.tableIdx(TableId::TypeDef); // Class
            { auto tok = rr.codedToken(kTypeDefOrRef, 3, 2);
              fields[f++] = tok.table; fields[f++] = tok.index; }
            break;
        case TableId::MemberRef: {
            auto tok = rr.codedToken(kMemberRefParent, 5, 3);
            fields[f++] = tok.table; fields[f++] = tok.index;
            fields[f++] = rr.strIdx();
            fields[f++] = rr.blobIdx();
            break;
        }
        case TableId::Constant: {
            fields[f++] = rr.u8(); rr.u8(); // Type + padding
            auto tok = rr.codedToken(kHasConstant, 3, 2);
            fields[f++] = tok.table; fields[f++] = tok.index;
            fields[f++] = rr.blobIdx();
            break;
        }
        case TableId::CustomAttribute: {
            auto tok = rr.codedToken(kHasCustomAttr,
                static_cast<size_t>(22), 5);
            fields[f++] = tok.table; fields[f++] = tok.index;
            auto tok2 = rr.codedToken(kCustomAttrType, 5, 3);
            fields[f++] = tok2.table; fields[f++] = tok2.index;
            fields[f++] = rr.blobIdx();
            break;
        }
        case TableId::ClassLayout:
            fields[f++] = rr.u16();  // PackingSize
            fields[f++] = rr.u32();  // ClassSize
            fields[f++] = rr.tableIdx(TableId::TypeDef);
            break;
        case TableId::StandAloneSig:
            fields[f++] = rr.blobIdx();
            break;
        case TableId::PropertyMap:
            fields[f++] = rr.tableIdx(TableId::TypeDef);
            fields[f++] = rr.tableIdx(TableId::Property);
            break;
        case TableId::Property:
            fields[f++] = rr.u16();
            fields[f++] = rr.strIdx();
            fields[f++] = rr.blobIdx();
            break;
        case TableId::MethodSemantics: {
            fields[f++] = rr.u16();
            fields[f++] = rr.tableIdx(TableId::MethodDef);
            auto tok = rr.codedToken(kHasSemantics, 2, 1);
            fields[f++] = tok.table; fields[f++] = tok.index;
            break;
        }
        case TableId::MethodImpl: {
            fields[f++] = rr.tableIdx(TableId::TypeDef);
            auto body = rr.codedToken(kMethodDefOrRef, 2, 1);
            fields[f++] = body.table; fields[f++] = body.index;
            auto decl = rr.codedToken(kMethodDefOrRef, 2, 1);
            fields[f++] = decl.table; fields[f++] = decl.index;
            break;
        }
        case TableId::ModuleRef:
            fields[f++] = rr.strIdx();
            break;
        case TableId::TypeSpec:
            fields[f++] = rr.blobIdx();
            break;
        case TableId::ImplMap: {
            fields[f++] = rr.u16();
            auto tok = rr.codedToken(kMemberForwarded, 2, 1);
            fields[f++] = tok.table; fields[f++] = tok.index;
            fields[f++] = rr.strIdx();
            fields[f++] = rr.tableIdx(TableId::ModuleRef);
            break;
        }
        case TableId::FieldRVA:
            fields[f++] = rr.u32();
            fields[f++] = rr.tableIdx(TableId::Field);
            break;
        case TableId::Assembly:
            fields[f++] = rr.u32();  // HashAlgId
            fields[f++] = rr.u16();  // MajorVersion
            fields[f++] = rr.u16();  // MinorVersion
            fields[f++] = rr.u16();  // BuildNumber
            fields[f++] = rr.u16();  // RevisionNumber
            fields[f++] = rr.u32();  // Flags
            fields[f++] = rr.blobIdx(); // PublicKey
            fields[f++] = rr.strIdx();  // Name
            fields[f++] = rr.strIdx();  // Culture
            break;
        case TableId::AssemblyRef:
            fields[f++] = rr.u16();
            fields[f++] = rr.u16();
            fields[f++] = rr.u16();
            fields[f++] = rr.u16();
            fields[f++] = rr.u32();
            fields[f++] = rr.blobIdx();
            fields[f++] = rr.strIdx();
            fields[f++] = rr.strIdx();
            fields[f++] = rr.blobIdx();
            break;
        case TableId::NestedClass:
            fields[f++] = rr.tableIdx(TableId::TypeDef);
            fields[f++] = rr.tableIdx(TableId::TypeDef);
            break;
        case TableId::GenericParam: {
            fields[f++] = rr.u16();  // Number
            fields[f++] = rr.u16();  // Flags
            auto tok = rr.codedToken(kTypeOrMethodDef, 2, 1);
            fields[f++] = tok.table; fields[f++] = tok.index;
            fields[f++] = rr.strIdx();
            break;
        }
        case TableId::MethodSpec: {
            auto tok = rr.codedToken(kMethodDefOrRef, 2, 1);
            fields[f++] = tok.table; fields[f++] = tok.index;
            fields[f++] = rr.blobIdx();
            break;
        }
        case TableId::GenericParamConstraint: {
            fields[f++] = rr.tableIdx(TableId::GenericParam);
            auto tok = rr.codedToken(kTypeDefOrRef, 3, 2);
            fields[f++] = tok.table; fields[f++] = tok.index;
            break;
        }
        case TableId::EventMap:
            fields[f++] = rr.tableIdx(TableId::TypeDef);
            fields[f++] = rr.tableIdx(TableId::Event);
            break;
        case TableId::Event: {
            fields[f++] = rr.u16();
            fields[f++] = rr.strIdx();
            auto tok = rr.codedToken(kTypeDefOrRef, 3, 2);
            fields[f++] = tok.table; fields[f++] = tok.index;
            break;
        }
        case TableId::File:
            fields[f++] = rr.u32();
            fields[f++] = rr.strIdx();
            fields[f++] = rr.blobIdx();
            break;
        case TableId::ManifestResource: {
            fields[f++] = rr.u32();
            fields[f++] = rr.u32();
            fields[f++] = rr.strIdx();
            auto tok = rr.codedToken(kImplementation, 3, 2);
            fields[f++] = tok.table; fields[f++] = tok.index;
            break;
        }
        case TableId::ExportedType: {
            fields[f++] = rr.u32();
            fields[f++] = rr.u32();
            fields[f++] = rr.strIdx();
            fields[f++] = rr.strIdx();
            auto tok = rr.codedToken(kImplementation, 3, 2);
            fields[f++] = tok.table; fields[f++] = tok.index;
            break;
        }
        default:
            // Tables not decoded (AssemblyProcessor, AssemblyOS, etc.) — skip
            // We can't know the row size without knowing what's here, so
            // just treat them as 0-row tables (they're always empty in practice).
            break;
        }
        (void)f;
    }
    return true;
}

// ─── Typed row accessors ──────────────────────────────────────────────────────

static const uint32_t* rowFields(const RawTable& tbl, uint32_t idx) {
    if (idx == 0 || idx > tbl.rowCount) return nullptr;
    size_t rowSize = tbl.rowSize;
    return reinterpret_cast<const uint32_t*>(
        tbl.data.data() + (idx - 1) * rowSize);
}

ModuleRow MetadataTables::module(uint32_t idx) const {
    const auto* f = rowFields(tables_[0], idx);
    if (!f) return {};
    return {static_cast<uint16_t>(f[0]), f[1], f[2], f[3], f[4]};
}

TypeRefRow MetadataTables::typeRef(uint32_t idx) const {
    const auto* f = rowFields(tables_[static_cast<size_t>(TableId::TypeRef)], idx);
    if (!f) return {};
    TypeRefRow r;
    r.resolutionScope = {static_cast<uint8_t>(f[0]), f[1]};
    r.name = f[2]; r.ns = f[3];
    return r;
}

TypeDefRow MetadataTables::typeDef(uint32_t idx) const {
    const auto* f = rowFields(tables_[static_cast<size_t>(TableId::TypeDef)], idx);
    if (!f) return {};
    TypeDefRow r;
    r.flags = f[0]; r.name = f[1]; r.ns = f[2];
    r.extends = {static_cast<uint8_t>(f[3]), f[4]};
    r.fieldList = f[5]; r.methodList = f[6];
    return r;
}

FieldRow MetadataTables::field(uint32_t idx) const {
    const auto* f = rowFields(tables_[static_cast<size_t>(TableId::Field)], idx);
    if (!f) return {};
    return {static_cast<uint16_t>(f[0]), f[1], f[2]};
}

MethodDefRow MetadataTables::methodDef(uint32_t idx) const {
    const auto* f = rowFields(tables_[static_cast<size_t>(TableId::MethodDef)], idx);
    if (!f) return {};
    return {f[0], static_cast<uint16_t>(f[1]),
            static_cast<uint16_t>(f[2]), f[3], f[4], f[5]};
}

ParamRow MetadataTables::param(uint32_t idx) const {
    const auto* f = rowFields(tables_[static_cast<size_t>(TableId::Param)], idx);
    if (!f) return {};
    return {static_cast<uint16_t>(f[0]), static_cast<uint16_t>(f[1]), f[2]};
}

InterfaceImplRow MetadataTables::interfaceImpl(uint32_t idx) const {
    const auto* f = rowFields(tables_[static_cast<size_t>(TableId::InterfaceImpl)], idx);
    if (!f) return {};
    InterfaceImplRow r;
    r.clazz = f[0];
    r.interface_ = {static_cast<uint8_t>(f[1]), f[2]};
    return r;
}

MemberRefRow MetadataTables::memberRef(uint32_t idx) const {
    const auto* f = rowFields(tables_[static_cast<size_t>(TableId::MemberRef)], idx);
    if (!f) return {};
    MemberRefRow r;
    r.clazz = {static_cast<uint8_t>(f[0]), f[1]};
    r.name = f[2]; r.signature = f[3];
    return r;
}

ConstantRow MetadataTables::constant(uint32_t idx) const {
    const auto* f = rowFields(tables_[static_cast<size_t>(TableId::Constant)], idx);
    if (!f) return {};
    ConstantRow r;
    r.type = static_cast<uint8_t>(f[0]);
    r.parent = {static_cast<uint8_t>(f[1]), f[2]};
    r.value = f[3];
    return r;
}

CustomAttributeRow MetadataTables::customAttribute(uint32_t idx) const {
    const auto* f = rowFields(tables_[static_cast<size_t>(TableId::CustomAttribute)], idx);
    if (!f) return {};
    CustomAttributeRow r;
    r.parent = {static_cast<uint8_t>(f[0]), f[1]};
    r.type   = {static_cast<uint8_t>(f[2]), f[3]};
    r.value  = f[4];
    return r;
}

ClassLayoutRow MetadataTables::classLayout(uint32_t idx) const {
    const auto* f = rowFields(tables_[static_cast<size_t>(TableId::ClassLayout)], idx);
    if (!f) return {};
    return {static_cast<uint16_t>(f[0]), f[1], f[2]};
}

StandAloneSigRow MetadataTables::standAloneSig(uint32_t idx) const {
    const auto* f = rowFields(tables_[static_cast<size_t>(TableId::StandAloneSig)], idx);
    if (!f) return {};
    return {f[0]};
}

PropertyRow MetadataTables::property(uint32_t idx) const {
    const auto* f = rowFields(tables_[static_cast<size_t>(TableId::Property)], idx);
    if (!f) return {};
    return {static_cast<uint16_t>(f[0]), f[1], f[2]};
}

MethodSemanticsRow MetadataTables::methodSemantics(uint32_t idx) const {
    const auto* f = rowFields(tables_[static_cast<size_t>(TableId::MethodSemantics)], idx);
    if (!f) return {};
    MethodSemanticsRow r;
    r.semantics = static_cast<uint16_t>(f[0]);
    r.method = f[1];
    r.association = {static_cast<uint8_t>(f[2]), f[3]};
    return r;
}

MethodImplRow MetadataTables::methodImpl(uint32_t idx) const {
    const auto* f = rowFields(tables_[static_cast<size_t>(TableId::MethodImpl)], idx);
    if (!f) return {};
    MethodImplRow r;
    r.clazz = f[0];
    r.methodBody = {static_cast<uint8_t>(f[1]), f[2]};
    r.methodDeclaration = {static_cast<uint8_t>(f[3]), f[4]};
    return r;
}

ModuleRefRow MetadataTables::moduleRef(uint32_t idx) const {
    const auto* f = rowFields(tables_[static_cast<size_t>(TableId::ModuleRef)], idx);
    if (!f) return {};
    return {f[0]};
}

TypeSpecRow MetadataTables::typeSpec(uint32_t idx) const {
    const auto* f = rowFields(tables_[static_cast<size_t>(TableId::TypeSpec)], idx);
    if (!f) return {};
    return {f[0]};
}

ImplMapRow MetadataTables::implMap(uint32_t idx) const {
    const auto* f = rowFields(tables_[static_cast<size_t>(TableId::ImplMap)], idx);
    if (!f) return {};
    ImplMapRow r;
    r.mappingFlags = static_cast<uint16_t>(f[0]);
    r.memberForwarded = {static_cast<uint8_t>(f[1]), f[2]};
    r.importName = f[3]; r.importScope = f[4];
    return r;
}

FieldRVARow MetadataTables::fieldRVA(uint32_t idx) const {
    const auto* f = rowFields(tables_[static_cast<size_t>(TableId::FieldRVA)], idx);
    if (!f) return {};
    return {f[0], f[1]};
}

AssemblyRow MetadataTables::assembly(uint32_t idx) const {
    const auto* f = rowFields(tables_[static_cast<size_t>(TableId::Assembly)], idx);
    if (!f) return {};
    AssemblyRow r;
    r.hashAlgId = f[0];
    r.majorVersion = static_cast<uint16_t>(f[1]);
    r.minorVersion = static_cast<uint16_t>(f[2]);
    r.buildNumber  = static_cast<uint16_t>(f[3]);
    r.revisionNumber = static_cast<uint16_t>(f[4]);
    r.flags = f[5]; r.publicKey = f[6]; r.name = f[7]; r.culture = f[8];
    return r;
}

AssemblyRefRow MetadataTables::assemblyRef(uint32_t idx) const {
    const auto* f = rowFields(tables_[static_cast<size_t>(TableId::AssemblyRef)], idx);
    if (!f) return {};
    AssemblyRefRow r;
    r.majorVersion = static_cast<uint16_t>(f[0]);
    r.minorVersion = static_cast<uint16_t>(f[1]);
    r.buildNumber  = static_cast<uint16_t>(f[2]);
    r.revisionNumber = static_cast<uint16_t>(f[3]);
    r.flags = f[4]; r.publicKeyOrToken = f[5];
    r.name = f[6]; r.culture = f[7]; r.hashValue = f[8];
    return r;
}

NestedClassRow MetadataTables::nestedClass(uint32_t idx) const {
    const auto* f = rowFields(tables_[static_cast<size_t>(TableId::NestedClass)], idx);
    if (!f) return {};
    return {f[0], f[1]};
}

GenericParamRow MetadataTables::genericParam(uint32_t idx) const {
    const auto* f = rowFields(tables_[static_cast<size_t>(TableId::GenericParam)], idx);
    if (!f) return {};
    GenericParamRow r;
    r.number = static_cast<uint16_t>(f[0]);
    r.flags  = static_cast<uint16_t>(f[1]);
    r.owner  = {static_cast<uint8_t>(f[2]), f[3]};
    r.name   = f[4];
    return r;
}

MethodSpecRow MetadataTables::methodSpec(uint32_t idx) const {
    const auto* f = rowFields(tables_[static_cast<size_t>(TableId::MethodSpec)], idx);
    if (!f) return {};
    MethodSpecRow r;
    r.method = {static_cast<uint8_t>(f[0]), f[1]};
    r.instantiation = f[2];
    return r;
}

GenericParamConstraintRow MetadataTables::genericParamConstraint(uint32_t idx) const {
    const auto* f = rowFields(tables_[static_cast<size_t>(TableId::GenericParamConstraint)], idx);
    if (!f) return {};
    GenericParamConstraintRow r;
    r.owner = f[0];
    r.constraint = {static_cast<uint8_t>(f[1]), f[2]};
    return r;
}

EventRow MetadataTables::event(uint32_t idx) const {
    const auto* f = rowFields(tables_[static_cast<size_t>(TableId::Event)], idx);
    if (!f) return {};
    EventRow r;
    r.flags = static_cast<uint16_t>(f[0]);
    r.name = f[1];
    r.eventType = {static_cast<uint8_t>(f[2]), f[3]};
    return r;
}

PropertyMapRow MetadataTables::propertyMap(uint32_t idx) const {
    const auto* f = rowFields(tables_[static_cast<size_t>(TableId::PropertyMap)], idx);
    if (!f) return {};
    return {f[0], f[1]};
}

EventMapRow MetadataTables::eventMap(uint32_t idx) const {
    const auto* f = rowFields(tables_[static_cast<size_t>(TableId::EventMap)], idx);
    if (!f) return {};
    return {f[0], f[1]};
}

FileRow MetadataTables::file(uint32_t idx) const {
    const auto* f = rowFields(tables_[static_cast<size_t>(TableId::File)], idx);
    if (!f) return {};
    return {f[0], f[1], f[2]};
}

ManifestResourceRow MetadataTables::manifestResource(uint32_t idx) const {
    const auto* f = rowFields(tables_[static_cast<size_t>(TableId::ManifestResource)], idx);
    if (!f) return {};
    ManifestResourceRow r;
    r.offset = f[0]; r.flags = f[1]; r.name = f[2];
    r.implementation = {static_cast<uint8_t>(f[3]), f[4]};
    return r;
}

ExportedTypeRow MetadataTables::exportedType(uint32_t idx) const {
    const auto* f = rowFields(tables_[static_cast<size_t>(TableId::ExportedType)], idx);
    if (!f) return {};
    ExportedTypeRow r;
    r.flags = f[0]; r.typeDefId = f[1];
    r.typeName = f[2]; r.typeNamespace = f[3];
    r.implementation = {static_cast<uint8_t>(f[4]), f[5]};
    return r;
}

// ─── Coded token decoders ─────────────────────────────────────────────────────

MetadataToken MetadataTables::decodeTypeDefOrRef(uint32_t coded) const {
    uint32_t tag = coded & 0x3;
    uint32_t idx = coded >> 2;
    if (tag >= 3) return {};
    return {kTypeDefOrRef[tag], idx};
}

MetadataToken MetadataTables::decodeResolutionScope(uint32_t coded) const {
    uint32_t tag = coded & 0x3;
    uint32_t idx = coded >> 2;
    if (tag >= 4) return {};
    return {kResolutionScope[tag], idx};
}

MetadataToken MetadataTables::decodeMemberRefParent(uint32_t coded) const {
    uint32_t tag = coded & 0x7;
    uint32_t idx = coded >> 3;
    if (tag >= 5) return {};
    return {kMemberRefParent[tag], idx};
}

MetadataToken MetadataTables::decodeHasCustomAttribute(uint32_t coded) const {
    uint32_t tag = coded & 0x1F;
    uint32_t idx = coded >> 5;
    if (tag >= 22) return {};
    return {kHasCustomAttr[tag], idx};
}

MetadataToken MetadataTables::decodeCustomAttributeType(uint32_t coded) const {
    uint32_t tag = coded & 0x7;
    uint32_t idx = coded >> 3;
    if (tag >= 5) return {};
    return {kCustomAttrType[tag], idx};
}

MetadataToken MetadataTables::decodeTypeOrMethodDef(uint32_t coded) const {
    uint32_t tag = coded & 0x1;
    uint32_t idx = coded >> 1;
    return {kTypeOrMethodDef[tag], idx};
}

MetadataToken MetadataTables::decodeMethodDefOrRef(uint32_t coded) const {
    uint32_t tag = coded & 0x1;
    uint32_t idx = coded >> 1;
    return {kMethodDefOrRef[tag], idx};
}

MetadataToken MetadataTables::decodeHasSemantics(uint32_t coded) const {
    uint32_t tag = coded & 0x1;
    uint32_t idx = coded >> 1;
    return {kHasSemantics[tag], idx};
}

MetadataToken MetadataTables::decodeMemberForwarded(uint32_t coded) const {
    uint32_t tag = coded & 0x1;
    uint32_t idx = coded >> 1;
    return {kMemberForwarded[tag], idx};
}

MetadataToken MetadataTables::decodeImplementation(uint32_t coded) const {
    uint32_t tag = coded & 0x3;
    uint32_t idx = coded >> 2;
    if (tag >= 3) return {};
    return {kImplementation[tag], idx};
}

MetadataToken MetadataTables::decodeHasConstant(uint32_t coded) const {
    uint32_t tag = coded & 0x3;
    uint32_t idx = coded >> 2;
    if (tag >= 3) return {};
    return {kHasConstant[tag], idx};
}

} // namespace cli_parser
} // namespace retdec
