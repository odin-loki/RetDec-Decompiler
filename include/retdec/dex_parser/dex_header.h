/**
 * @file include/retdec/dex_parser/dex_header.h
 * @brief DEX file header, index tables, and low-level binary reader.
 *
 * Covers the Dalvik Executable format as specified in:
 *   - Android DEX format reference (dex-format.html, AOSP)
 *   - Compact DEX (CDex) extensions used in ART OAT files
 *   - DEX 035 (Dalvik), 036 (ART 5.0), 037 (ART 7.0), 038 (ART 8.0),
 *     039 (ART 9.0+), 040 (compact DEX)
 *
 * ## DEX file layout (linear)
 *
 *   header_item              § 4.1
 *   string_id_item[]         § 4.2.1  — offsets into data section
 *   type_id_item[]           § 4.2.2  — index into string_ids
 *   proto_id_item[]          § 4.2.3  — shorty idx, return type idx, params
 *   field_id_item[]          § 4.2.4  — class idx, type idx, name idx
 *   method_id_item[]         § 4.2.5  — class idx, proto idx, name idx
 *   class_def_item[]         § 4.2.6  — full class descriptor
 *   call_site_id_item[]      § 4.2.7  (DEX 038+)
 *   method_handle_item[]     § 4.2.9  (DEX 038+)
 *   data section             (variable-size items referenced by above)
 */

#ifndef RETDEC_DEX_PARSER_DEX_HEADER_H
#define RETDEC_DEX_PARSER_DEX_HEADER_H

#include <cstdint>
#include <stdexcept>
#include <string>
#include <vector>

namespace retdec {
namespace dex_parser {

// ─── Parse error ─────────────────────────────────────────────────────────────

class DexParseError : public std::runtime_error {
public:
    explicit DexParseError(const std::string& msg)
        : std::runtime_error("DEX parse error: " + msg) {}
};

// ─── Little-endian binary reader ──────────────────────────────────────────────

/**
 * @brief Bounds-checked little-endian reader for DEX/CDex binary data.
 */
class DexReader {
public:
    DexReader(const uint8_t* data, size_t size);

    size_t pos()       const { return pos_; }
    size_t size()      const { return size_; }
    size_t remaining() const { return size_ - pos_; }

    void seek(size_t offset);
    void skip(size_t n);

    uint8_t  u1();
    uint16_t u2();
    uint32_t u4();
    uint64_t u8();
    int8_t   s1();
    int16_t  s2();
    int32_t  s4();
    int64_t  s8();
    float    f4();
    double   f8();

    /// Read ULEB128 (unsigned LEB128) — used throughout DEX.
    uint32_t uleb128();
    /// Read SLEB128 (signed LEB128).
    int32_t  sleb128();
    /// Read ULEB128p1 — ULEB128 value then subtract 1.
    int32_t  uleb128p1();

    /// Read a modified UTF-8 string (MUTF-8, like JVM).
    std::string mutf8(uint32_t len);

    std::vector<uint8_t> bytes(size_t n);

    const uint8_t* data() const { return data_; }
    const uint8_t* ptr()  const { return data_ + pos_; }

private:
    const uint8_t* data_;
    size_t         size_;
    size_t         pos_ = 0;

    void check(size_t n) const;
};

// ─── DEX version constants ────────────────────────────────────────────────────

enum class DexVersion : uint32_t {
    V035 = 35,  ///< Dalvik 035 (original)
    V036 = 36,  ///< ART 5.0
    V037 = 37,  ///< ART 7.0
    V038 = 38,  ///< ART 8.0 (invoke-polymorphic, invoke-custom, call sites)
    V039 = 39,  ///< ART 9.0+
    V040 = 40,  ///< Compact DEX (CDEX) used in ART OAT
    Unknown = 0,
};

// ─── DEX header_item ─────────────────────────────────────────────────────────

static constexpr uint8_t kDexMagic[8]  = {0x64,0x65,0x78,0x0a,'0','3','5',0x00};
static constexpr uint8_t kCdexMagic[4] = {0x63,0x64,0x65,0x78}; // "cdex"
static constexpr uint32_t kEndianConst     = 0x12345678u;
static constexpr uint32_t kEndianConstSwap = 0x78563412u;

struct DexHeader {
    uint8_t  magic[8];         ///< "dex\n035\0" or similar
    uint32_t checksum;         ///< Adler-32 over rest of file
    uint8_t  sha1[20];         ///< SHA-1 over rest of file
    uint32_t fileSize;         ///< Total file size in bytes
    uint32_t headerSize;       ///< Size of this header (0x70 for standard DEX)
    uint32_t endianTag;        ///< 0x12345678 for LE (normal), 0x78563412 for BE
    uint32_t linkSize;         ///< Size of link section (0 for static)
    uint32_t linkOff;          ///< Offset of link section (0 for static)
    uint32_t mapOff;           ///< Offset of map list
    uint32_t stringIdsSize;    ///< Count of string_id_items
    uint32_t stringIdsOff;     ///< Offset of string_ids list
    uint32_t typeIdsSize;      ///< Count of type_id_items
    uint32_t typeIdsOff;       ///< Offset of type_ids list
    uint32_t protoIdsSize;     ///< Count of proto_id_items
    uint32_t protoIdsOff;      ///< Offset of proto_ids list
    uint32_t fieldIdsSize;     ///< Count of field_id_items
    uint32_t fieldIdsOff;      ///< Offset of field_ids list
    uint32_t methodIdsSize;    ///< Count of method_id_items
    uint32_t methodIdsOff;     ///< Offset of method_ids list
    uint32_t classDefsSize;    ///< Count of class_def_items
    uint32_t classDefsOff;     ///< Offset of class_defs list
    uint32_t dataSize;         ///< Size of data section
    uint32_t dataOff;          ///< Offset of data section

    DexVersion version() const;
    bool isCompact() const;
    bool isBigEndian() const { return endianTag == kEndianConstSwap; }
};

// ─── Index table entries ──────────────────────────────────────────────────────

struct StringId {
    uint32_t stringDataOff;  ///< Offset of string_data_item in data section
};

struct TypeId {
    uint32_t descriptorIdx;  ///< Index into string_ids (class descriptor)
};

struct ProtoId {
    uint32_t shortyIdx;      ///< Index into string_ids (shorty descriptor)
    uint32_t returnTypeIdx;  ///< Index into type_ids (return type)
    uint32_t parametersOff;  ///< Offset of type_list for parameters (0 = none)
};

struct FieldId {
    uint16_t classIdx;       ///< Index into type_ids (defining class)
    uint16_t typeIdx;        ///< Index into type_ids (field type)
    uint32_t nameIdx;        ///< Index into string_ids (field name)
};

struct MethodId {
    uint16_t classIdx;       ///< Index into type_ids (defining class)
    uint16_t protoIdx;       ///< Index into proto_ids (method signature)
    uint32_t nameIdx;        ///< Index into string_ids (method name)
};

/// Flags for class_def_item.accessFlags and field/method accessFlags.
enum DexAccessFlags : uint32_t {
    ACC_PUBLIC       = 0x00001,
    ACC_PRIVATE      = 0x00002,
    ACC_PROTECTED    = 0x00004,
    ACC_STATIC       = 0x00008,
    ACC_FINAL        = 0x00010,
    ACC_SYNCHRONIZED = 0x00020,
    ACC_VOLATILE     = 0x00040,
    ACC_BRIDGE       = 0x00040,
    ACC_TRANSIENT    = 0x00080,
    ACC_VARARGS      = 0x00080,
    ACC_NATIVE       = 0x00100,
    ACC_INTERFACE    = 0x00200,
    ACC_ABSTRACT     = 0x00400,
    ACC_STRICT       = 0x00800,
    ACC_SYNTHETIC    = 0x01000,
    ACC_ANNOTATION   = 0x02000,
    ACC_ENUM         = 0x04000,
    ACC_CONSTRUCTOR  = 0x10000,
    ACC_DECLARED_SYNCHRONIZED = 0x20000,
};

struct ClassDef {
    uint32_t classIdx;           ///< Index into type_ids (this class)
    uint32_t accessFlags;        ///< DexAccessFlags combination
    uint32_t superclassIdx;      ///< Index into type_ids, NO_INDEX if none
    uint32_t interfacesOff;      ///< Offset of type_list (interfaces), 0=none
    uint32_t sourceFileIdx;      ///< Index into string_ids, NO_INDEX if none
    uint32_t annotationsOff;     ///< Offset of annotations_directory_item, 0=none
    uint32_t classDataOff;       ///< Offset of class_data_item, 0=none
    uint32_t staticValuesOff;    ///< Offset of encoded_array_item, 0=none

    static constexpr uint32_t NO_INDEX = 0xFFFFFFFFu;
};

struct CallSiteId {
    uint32_t callSiteOff;    ///< Offset of call_site_item (DEX 038+)
};

struct MethodHandle {
    uint16_t methodHandleType; ///< MethodHandleType enum
    uint16_t unused1;
    uint16_t fieldOrMethodId;  ///< Index into field_ids or method_ids
    uint16_t unused2;
};

// ─── class_data_item sub-structures ─────────────────────────────────────────

struct EncodedField {
    uint32_t fieldIdxDiff;   ///< ULEB128 index diff (cumulative from prev)
    uint32_t accessFlags;    ///< ULEB128 DexAccessFlags
    uint32_t fieldIdx = 0;   ///< Resolved absolute index (filled by parser)
};

struct EncodedMethod {
    uint32_t methodIdxDiff;  ///< ULEB128 index diff (cumulative from prev)
    uint32_t accessFlags;    ///< ULEB128 DexAccessFlags
    uint32_t codeOff;        ///< ULEB128 offset of code_item, 0=abstract/native
    uint32_t methodIdx = 0;  ///< Resolved absolute index
};

struct ClassData {
    std::vector<EncodedField>  staticFields;
    std::vector<EncodedField>  instanceFields;
    std::vector<EncodedMethod> directMethods;
    std::vector<EncodedMethod> virtualMethods;
};

// ─── code_item ────────────────────────────────────────────────────────────────

struct TryItem {
    uint32_t startAddr;      ///< Start PC (code units, 2 bytes each)
    uint16_t insnCount;      ///< Instruction count of covered range
    uint16_t handlerOff;     ///< Offset into encoded_catch_handler_list
};

struct CatchHandler {
    int32_t  typeIdx;        ///< Type index, -1 = catch-all
    uint32_t addr;           ///< Handler PC (code units)
};

struct EncodedCatchHandlerList {
    std::vector<std::vector<CatchHandler>> handlers;
    std::vector<uint32_t>                 catchAllAddrs; ///< catch-all addr per handler
};

struct CodeItem {
    uint16_t registersSize;  ///< Total register count (vN)
    uint16_t insSize;        ///< Number of parameter words
    uint16_t outsSize;       ///< Max outgoing argument words
    uint16_t triesSize;      ///< Number of try_item records
    uint32_t debugInfoOff;   ///< Offset of debug_info_item
    uint32_t insnsSize;      ///< Instruction list size in code units (u16 each)
    std::vector<uint16_t>    insns;         ///< Raw instruction words
    std::vector<TryItem>     tries;
    EncodedCatchHandlerList  handlers;
};

// ─── DEX file container ───────────────────────────────────────────────────────

/**
 * @brief Parsed DEX file — all index tables loaded into memory.
 *
 * Provides string/type/proto/field/method resolution by index.
 */
class DexFile {
public:
    /// Parse a DEX file from raw bytes. Throws DexParseError on failure.
    static DexFile parse(const uint8_t* data, size_t size);
    static DexFile parse(const std::vector<uint8_t>& data);

    const DexHeader& header() const { return header_; }
    DexVersion version()      const { return header_.version(); }

    uint32_t stringCount()  const { return static_cast<uint32_t>(strings_.size()); }
    uint32_t typeCount()    const { return static_cast<uint32_t>(typeIds_.size()); }
    uint32_t protoCount()   const { return static_cast<uint32_t>(protoIds_.size()); }
    uint32_t fieldCount()   const { return static_cast<uint32_t>(fieldIds_.size()); }
    uint32_t methodCount()  const { return static_cast<uint32_t>(methodIds_.size()); }
    uint32_t classCount()   const { return static_cast<uint32_t>(classDefs_.size()); }

    // String resolution
    const std::string& string(uint32_t idx)     const;
    const std::string& typeName(uint32_t typeIdx) const; ///< Resolved class descriptor
    std::string        typeDescriptor(uint32_t typeIdx) const;

    // Index table accessors
    const TypeId&   typeId(uint32_t idx)   const { return typeIds_.at(idx); }
    const ProtoId&  protoId(uint32_t idx)  const { return protoIds_.at(idx); }
    const FieldId&  fieldId(uint32_t idx)  const { return fieldIds_.at(idx); }
    const MethodId& methodId(uint32_t idx) const { return methodIds_.at(idx); }
    const ClassDef& classDef(uint32_t idx) const { return classDefs_.at(idx); }

    // Resolution helpers
    std::string fieldClass(uint32_t fieldIdx)  const;
    std::string fieldType(uint32_t fieldIdx)   const;
    std::string fieldName(uint32_t fieldIdx)   const;
    std::string methodClass(uint32_t methodIdx) const;
    std::string methodName(uint32_t methodIdx)  const;
    std::string methodProto(uint32_t methodIdx) const; ///< "(II)V" style

    // Read class_data_item at offset.
    ClassData readClassData(uint32_t offset) const;

    // Read code_item at offset.
    CodeItem readCodeItem(uint32_t offset) const;

    // Read type_list at offset (returns type indices).
    std::vector<uint32_t> readTypeList(uint32_t offset) const;

    // Raw data access for sub-parsers.
    const uint8_t* rawData()  const { return data_.data(); }
    size_t         rawSize()  const { return data_.size(); }

private:
    DexHeader                header_{};
    std::vector<std::string> strings_;   ///< Resolved MUTF-8 strings
    std::vector<TypeId>      typeIds_;
    std::vector<ProtoId>     protoIds_;
    std::vector<FieldId>     fieldIds_;
    std::vector<MethodId>    methodIds_;
    std::vector<ClassDef>    classDefs_;
    std::vector<uint8_t>     data_;      ///< Full file copy

    void parseHeader(DexReader& r);
    void parseStringIds(DexReader& r);
    void parseTypeIds(DexReader& r);
    void parseProtoIds(DexReader& r);
    void parseFieldIds(DexReader& r);
    void parseMethodIds(DexReader& r);
    void parseClassDefs(DexReader& r);
};

} // namespace dex_parser
} // namespace retdec

#endif // RETDEC_DEX_PARSER_DEX_HEADER_H
