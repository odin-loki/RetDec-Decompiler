/**
 * @file include/retdec/serial_detect/serial_detect.h
 * @brief Serialisation Framework Detector — Stage 38.
 *
 * Identifies compiled serialisation frameworks (Protobuf, FlatBuffers,
 * MessagePack, CBOR, JSON libraries, XML parsers) and reconstructs their
 * schemas from binary functions.
 *
 * ## Detected frameworks
 *
 * ### Google Protocol Buffers (protobuf 2/3)
 *
 * Detection strategy:
 *   1. **Symbol matching** — `SerializeToString`, `ParseFromString`,
 *      `SerializeToArray`, `ParseFromArray`, `ByteSizeLong` in symbol table.
 *   2. **`_has_bits_` array** — generated classes store presence bitmask in a
 *      fixed-size `uint32_t _has_bits_[N]` field at a predictable struct offset.
 *   3. **Varint encoding loop** — the pattern `byte = (value & 0x7F) | 0x80;
 *      value >>= 7;` repeated until `value == 0`.
 *   4. **Wire-type tag construction** — `tag = (field_number << 3) | wire_type`
 *      where `wire_type ∈ {0,1,2,5}`.
 *   5. **Schema reconstruction** — for each encode/decode call-site, recover
 *      `field_number` and `wire_type` from constant operands; map wire_type to
 *      C++/proto types; emit synthetic or symbol-derived field names.
 *
 * ### FlatBuffers
 *
 * Detection strategy:
 *   1. **Vtable lookup pattern** — `int16_t offset = *(vtable + slot*2)`;
 *      followed by `if (offset) return base + offset;`.
 *   2. **Offset-based field access** — no separate parse step; fields read
 *      directly as `*(base_ptr + field_offset)`.
 *   3. **Builder pattern** — `StartTable`, `AddElement`, `EndTable` call
 *      sequences in the IR.
 *   4. **Verifier** — `FlatBufferBuilder::Verify` call or inline size-check
 *      loop at the front of a receive function.
 *
 * ### MessagePack
 *
 * Detection strategy:
 *   - **Type-tag-value pattern**: read one byte → switch on
 *     `{0x00-0x7F=positive fixint, 0x80-0x8F=fixmap, 0x90-0x9F=fixarray,
 *      0xa0-0xbf=fixstr, 0xc0-0xdf=special, 0xe0-0xff=negative fixint}`.
 *   - Fixed-width reads following format-byte dispatch (1/2/4/8 bytes).
 *   - The switch has ≥ 8 cases with contiguous type-byte ranges.
 *
 * ### CBOR (RFC 7049 / 8949)
 *
 * Detection strategy:
 *   - **Major-type extraction**: `major = (byte >> 5) & 7` (3-bit field).
 *   - **Additional info**: `additional = byte & 0x1F`.
 *   - Switch on major type → {uint, negint, bytes, text, array, map, tag, simple}.
 *   - Additional-info decode: `24→1-byte, 25→2-byte, 26→4-byte, 27→8-byte`.
 *
 * ### JSON (library identification)
 *
 * Generic JSON detection:
 *   - `parse_value → parse_object / parse_array` mutual-recursion call graph.
 *   - Switch on `{'{', '[', '"', 't', 'f', 'n', digit, '-'}` for value type.
 *
 * Library-specific fingerprints:
 *   - **RapidJSON**: `GenericDocument::Parse`, `Value::GetType`,
 *     `GenericStringRef`, `kObjectType` / `kArrayType` constant usage.
 *   - **nlohmann/json**: template instantiation patterns, `basic_json::parse`,
 *     `detail::json_sax_dom_parser`.
 *   - **simdjson**: SIMD structural character search (`_mm_cmpeq_epi8` with
 *     `'{', '[', '}', ']', ':', ','` masks); `ondemand::` namespace symbols.
 *   - **cJSON**: `cJSON_Parse`, `cJSON_GetObjectItem`, struct with `{next,
 *     prev, child, type, valuestring, valueint, valuedouble}` layout.
 *
 * ### XML (library identification)
 *
 * Generic XML detection:
 *   - Tag-stack push/pop around element content.
 *   - Character-level `<`, `>`, `&`, `/` scanning.
 *
 * Library-specific fingerprints:
 *   - **libxml2**: `xmlReadMemory`, `xmlDocGetRootElement`, `xmlFreeDoc`,
 *     `xmlParseFile`; `_xmlNode` struct layout.
 *   - **Expat**: `XML_ParserCreate`, `XML_SetElementHandler`,
 *     `XML_SetCharacterDataHandler`; SAX callback registration pattern.
 *   - **TinyXML / TinyXML-2**: `XMLDocument::Parse`, `XMLElement::FirstChild`,
 *     `XMLElement::Attribute`.
 *   - **RapidXML**: `xml_document::parse<>`, template parse function with
 *     char-pointer in/out parameter.
 *
 * ## Schema reconstruction
 *
 * ### Protobuf `.proto` emission
 *
 * For each detected protobuf class:
 *   1. Walk all encode/decode call-sites to collect `(field_number, wire_type)`.
 *   2. Map wire_type → proto scalar type:
 *        0→varint (int32/int64/bool/enum), 1→fixed64 (double/fixed64),
 *        2→length-delimited (bytes/string/embedded message),
 *        5→fixed32 (float/fixed32).
 *   3. Derive field names from symbol table (getter/setter names), or use
 *      synthetic `field_N` names.
 *   4. Detect repeated fields from loop-over-repeated-call patterns.
 *   5. Emit `message ClassName { field_type field_name = field_number; … }`.
 *
 * ### FlatBuffers `.fbs` emission
 *
 * For each detected FlatBuffers table:
 *   1. Walk vtable slot accesses to collect `(slot_index, access_width)`.
 *   2. Map access_width → type: 1→bool/byte, 2→short, 4→int/float, 8→long/double.
 *   3. Emit `table TableName { field_N : Type; … }` with a `root_type` if
 *      identified.
 */

#ifndef RETDEC_SERIAL_DETECT_H
#define RETDEC_SERIAL_DETECT_H

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace retdec {
namespace ssa { class SSAFunction; }
} // namespace retdec

namespace retdec {
namespace serial_detect {

// ─── Enumerations ─────────────────────────────────────────────────────────────

/// Top-level serialisation framework categories.
enum class SerialFramework : uint8_t {
    Unknown,
    Protobuf,
    FlatBuffers,
    MessagePack,
    CBOR,
    JSON,
    XML,
};

/// Specific library variant within a framework.
enum class SerialLibrary : uint8_t {
    Unknown,
    // Protobuf
    Protobuf2,          ///< proto2 (required/optional/repeated)
    Protobuf3,          ///< proto3 (all fields optional by default)
    // JSON
    JSON_RapidJSON,
    JSON_nlohmann,
    JSON_simdjson,
    JSON_cJSON,
    JSON_Generic,       ///< hand-rolled recursive descent
    // XML
    XML_libxml2,
    XML_Expat,
    XML_TinyXML,
    XML_RapidXML,
    XML_Generic,
    // Others (single library per format)
    FlatBuffers_Official,
    MessagePack_msgpack_c,
    MessagePack_Generic,
    CBOR_Generic,
};

/// Wire type for protobuf fields.
enum class ProtoWireType : uint8_t {
    Varint         = 0, ///< int32, int64, uint32, uint64, sint32, sint64, bool, enum
    Fixed64        = 1, ///< fixed64, sfixed64, double
    LengthDelimited= 2, ///< string, bytes, embedded messages, packed repeated fields
    StartGroup     = 3, ///< deprecated groups
    EndGroup       = 4, ///< deprecated groups
    Fixed32        = 5, ///< fixed32, sfixed32, float
    Unknown        = 0xFF,
};

/// Protobuf scalar type recovered from wire type + width.
enum class ProtoScalarType : uint8_t {
    Unknown,
    Bool,
    Int32, Int64,
    UInt32, UInt64,
    SInt32, SInt64,    ///< zigzag encoded
    Fixed32, Fixed64,
    SFixed32, SFixed64,
    Float, Double,
    String, Bytes,
    Message,           ///< nested message (length-delimited)
    Enum,
};

/// Cardinality of a protobuf field.
enum class ProtoCardinality : uint8_t {
    Optional,   ///< proto3 default / proto2 optional
    Required,   ///< proto2 required
    Repeated,   ///< list of values
    Map,        ///< map<K,V> (desugared to a repeated message)
};

// ─── Protobuf schema structures ───────────────────────────────────────────────

/// A single protobuf field recovered from binary.
struct ProtoField {
    uint32_t        number      = 0;
    std::string     name;               ///< From symbol table or "field_N"
    ProtoWireType   wireType    = ProtoWireType::Unknown;
    ProtoScalarType scalarType  = ProtoScalarType::Unknown;
    ProtoCardinality cardinality = ProtoCardinality::Optional;
    bool            hasPresenceBit = false; ///< proto2 has_field() pattern
    std::string     nestedMessageName;      ///< if scalarType == Message

    std::string protoTypeName() const;
    std::string toString() const;
};

/// A reconstructed protobuf message.
struct ProtoMessage {
    std::string               name;
    std::vector<ProtoField>   fields;
    std::vector<std::string>  nestedMessageNames;
    std::string               sourceFunction; ///< Function where detected

    bool isValid() const { return !fields.empty(); }
};

/// Top-level protobuf schema (may contain multiple messages).
struct ProtoSchema {
    std::string                    packageName;   ///< From namespace analysis
    std::string                    syntaxVersion; ///< "proto2" or "proto3"
    std::vector<ProtoMessage>      messages;
    std::vector<std::string>       imports;

    bool isEmpty() const { return messages.empty(); }

    /// Emit as a .proto text file.
    std::string emitProto() const;
};

// ─── FlatBuffers schema structures ────────────────────────────────────────────

/// A FlatBuffers field recovered from vtable slot analysis.
struct FbsField {
    uint32_t    slot    = 0;    ///< vtable slot index
    std::string name;           ///< From symbol table or "field_N"
    uint8_t     accessBytes = 0;///< 1/2/4/8 bytes
    bool        isVector    = false;
    bool        isString    = false;
    std::string typeName;       ///< Inferred type name

    std::string fbsTypeName() const;
};

/// A FlatBuffers table or struct recovered from binary.
struct FbsTable {
    std::string              name;
    std::vector<FbsField>    fields;
    bool                     isStruct = false;  ///< struct (fixed layout) vs table
    std::string              sourceFunction;

    std::string emitFbs() const;
};

/// Top-level FlatBuffers schema.
struct FbsSchema {
    std::vector<FbsTable>   tables;
    std::string             rootType;  ///< root_type declaration

    bool isEmpty() const { return tables.empty(); }
    std::string emitFbs() const;
};

// ─── Detection result ─────────────────────────────────────────────────────────

/// Detection result for a single function or module.
struct SerialResult {
    SerialFramework framework  = SerialFramework::Unknown;
    SerialLibrary   library    = SerialLibrary::Unknown;
    float           confidence = 0.0f;

    /// Populated when framework == Protobuf
    std::optional<ProtoSchema> protoSchema;
    /// Populated when framework == FlatBuffers
    std::optional<FbsSchema>   fbsSchema;

    /// Source function(s) providing the strongest evidence.
    std::vector<std::string> evidenceFunctions;

    /// Human-readable evidence summary.
    std::string evidenceSummary;

    std::string frameworkName() const noexcept;
    std::string libraryName()   const noexcept;
    std::string toString()      const;
    bool        isValid()       const { return framework != SerialFramework::Unknown; }
};

// ─── Wire-format evidence helpers ─────────────────────────────────────────────

/// Evidence of the protobuf varint encoding loop.
struct VarintEvidence {
    bool  found       = false;
    float confidence  = 0.0f;
    int   loopCount   = 0;   ///< occurrences of the 7-bit shift pattern
};

/// Evidence of the protobuf field-tag construction.
struct TagEvidence {
    bool     found       = false;
    uint32_t fieldNumber = 0;
    ProtoWireType wireType = ProtoWireType::Unknown;
};

/// Evidence of the FlatBuffers vtable lookup pattern.
struct VtableEvidence {
    bool    found          = false;
    float   confidence     = 0.0f;
    int     slotCount      = 0;  ///< distinct vtable slots accessed
    bool    hasBuilderCall = false;
    bool    hasVerifier    = false;
};

/// Evidence of a MessagePack type-switch.
struct MsgpackEvidence {
    bool  found       = false;
    float confidence  = 0.0f;
    int   caseCount   = 0;   ///< switch cases on format byte
    bool  hasFixint   = false;
    bool  hasFixmap   = false;
    bool  hasFixarray = false;
    bool  hasFixstr   = false;
};

/// Evidence of CBOR major-type decoding.
struct CborEvidence {
    bool  found          = false;
    float confidence     = 0.0f;
    bool  hasMajorShift  = false;  ///< (byte >> 5) & 7 pattern
    bool  hasAdditional  = false;  ///< byte & 0x1F pattern
    bool  hasExtendedLen = false;  ///< 24/25/26/27 additional-info cases
};

/// Evidence of a JSON recursive-descent parser.
struct JsonParserEvidence {
    bool  found           = false;
    float confidence      = 0.0f;
    bool  hasValueSwitch  = false;  ///< switch on '{', '[', '"', etc.
    bool  hasMutualRecurs = false;  ///< parse_value ↔ parse_object/parse_array
    bool  hasStringEscape = false;  ///< backslash escape handling
    bool  hasUnicodeDecod = false;  ///< \uXXXX decoding
};

/// Library-specific JSON evidence.
struct JsonLibraryEvidence {
    SerialLibrary library     = SerialLibrary::Unknown;
    float         confidence  = 0.0f;
    bool  hasRapidJsonSymbols = false;
    bool  hasNlohmannSymbols  = false;
    bool  hasSimdjsonSimd     = false;
    bool  hasCJsonSymbols     = false;
};

/// Evidence of an XML parser.
struct XmlParserEvidence {
    bool  found          = false;
    float confidence     = 0.0f;
    bool  hasTagStack    = false;  ///< push/pop around content
    bool  hasAttrParsing = false;
    bool  hasSaxCallback = false;  ///< SAX callback registration
    bool  hasDomTree     = false;  ///< DOM tree construction
};

// ─── Per-format detectors ─────────────────────────────────────────────────────

/// Base interface for serialisation framework detectors.
class ISerialDetector {
public:
    virtual ~ISerialDetector() = default;

    /**
     * @brief Analyse a single function for evidence of this framework.
     * @param fn         The SSA function to analyse.
     * @param symTable   Symbol names visible at this function's address.
     * @return           Detection result; confidence = 0 if not detected.
     */
    virtual SerialResult detect(
        const ssa::SSAFunction& fn,
        const std::unordered_set<std::string>& symTable) const = 0;

    virtual SerialFramework framework() const noexcept = 0;
};

// ── Protobuf ──────────────────────────────────────────────────────────────────

/**
 * @brief Protobuf 2/3 detector and schema reconstructor.
 *
 * Detection is multi-layered:
 *   1. Symbol-table lookup for `SerializeToString`, `ParseFromString`, etc.
 *   2. `_has_bits_` array presence at struct offset 0 or adjacent.
 *   3. Varint encode/decode loop pattern.
 *   4. Field-tag arithmetic `(n << 3) | wt`.
 *
 * Schema reconstruction:
 *   For each detected message class, collects `(field_number, wire_type)` from
 *   constant operands in tag-construction instructions, then emits `.proto`.
 */
class ProtobufDetector : public ISerialDetector {
public:
    SerialResult    detect(const ssa::SSAFunction& fn,
                           const std::unordered_set<std::string>& sym) const override;
    SerialFramework framework() const noexcept override { return SerialFramework::Protobuf; }

    /// Reconstruct the full ProtoSchema from a set of functions that share
    /// the same protobuf class (identified by their common `_has_bits_` offset).
    ProtoSchema reconstructSchema(
        const std::vector<const ssa::SSAFunction*>& classFunctions,
        const std::unordered_set<std::string>& symTable) const;

    /// Emit a `.proto` file from a reconstructed schema.
    static std::string emitProto(const ProtoSchema& schema);

private:
    VarintEvidence  detectVarint(const ssa::SSAFunction& fn) const;
    TagEvidence     detectTag(const ssa::SSAFunction& fn) const;
    bool            hasHasBitsArray(const ssa::SSAFunction& fn) const;
    bool            hasSerializeSymbol(const std::unordered_set<std::string>& sym) const;
    bool            hasParseSymbol(const std::unordered_set<std::string>& sym) const;
    SerialLibrary   detectVersion(const ssa::SSAFunction& fn,
                                   const std::unordered_set<std::string>& sym) const;

    ProtoField      recoverField(uint32_t fieldNum, ProtoWireType wt,
                                  const std::string& symbolicName) const;
    ProtoScalarType wireTypeToScalar(ProtoWireType wt, uint8_t accessBytes) const;
};

// ── FlatBuffers ───────────────────────────────────────────────────────────────

/**
 * @brief FlatBuffers detector and schema reconstructor.
 *
 * Detection:
 *   - Vtable offset lookup: `(int16_t)*(vtab_ptr + slot*2)` with zero-check.
 *   - Builder API call chain: `StartTable` → `AddElement<T>` → `EndTable`.
 *   - Verifier call or inline bounds-check at function entry.
 *
 * Schema reconstruction:
 *   - Each distinct vtable slot access → one FbsField.
 *   - Access width (1/2/4/8 bytes) → FlatBuffers scalar type.
 *   - Vector accesses (pointer + length pair) → vector field.
 */
class FlatBuffersDetector : public ISerialDetector {
public:
    SerialResult    detect(const ssa::SSAFunction& fn,
                           const std::unordered_set<std::string>& sym) const override;
    SerialFramework framework() const noexcept override { return SerialFramework::FlatBuffers; }

    FbsSchema reconstructSchema(
        const std::vector<const ssa::SSAFunction*>& tableFunctions,
        const std::unordered_set<std::string>& symTable) const;

    static std::string emitFbs(const FbsSchema& schema);

private:
    VtableEvidence  detectVtable(const ssa::SSAFunction& fn) const;
    bool            hasBuilderPattern(const ssa::SSAFunction& fn) const;
    bool            hasVerifier(const ssa::SSAFunction& fn,
                                 const std::unordered_set<std::string>& sym) const;
    std::vector<FbsField> recoverFields(const ssa::SSAFunction& fn) const;
};

// ── MessagePack ───────────────────────────────────────────────────────────────

/**
 * @brief MessagePack detector.
 *
 * Key evidence: switch statement on a format-byte with ≥ 8 cases covering
 * the MessagePack format map:
 *   0x00-0x7F  positive fixint
 *   0x80-0x8F  fixmap
 *   0x90-0x9F  fixarray
 *   0xa0-0xbf  fixstr
 *   0xc0       nil
 *   0xc2/0xc3  false/true
 *   0xca/0xcb  float32/float64
 *   0xd0-0xd3  int8/16/32/64
 */
class MessagePackDetector : public ISerialDetector {
public:
    SerialResult    detect(const ssa::SSAFunction& fn,
                           const std::unordered_set<std::string>& sym) const override;
    SerialFramework framework() const noexcept override { return SerialFramework::MessagePack; }

private:
    MsgpackEvidence detectSwitch(const ssa::SSAFunction& fn) const;
    bool            hasFixedWidthRead(const ssa::SSAFunction& fn) const;
    SerialLibrary   detectLibrary(const std::unordered_set<std::string>& sym) const;
};

// ── CBOR ──────────────────────────────────────────────────────────────────────

/**
 * @brief CBOR (RFC 8949) detector.
 *
 * Key invariants:
 *   1. `major = (initial_byte >> 5) & 7`  — 3-bit right-shift + mask.
 *   2. `additional = initial_byte & 0x1F` — 5-bit lower mask.
 *   3. Extended-length decode for additional ∈ {24, 25, 26, 27}.
 *   4. Recursive item decode (CBOR is self-describing and nestable).
 */
class CBORDetector : public ISerialDetector {
public:
    SerialResult    detect(const ssa::SSAFunction& fn,
                           const std::unordered_set<std::string>& sym) const override;
    SerialFramework framework() const noexcept override { return SerialFramework::CBOR; }

private:
    CborEvidence    detectMajorType(const ssa::SSAFunction& fn) const;
    bool            hasAdditionalInfo(const ssa::SSAFunction& fn) const;
    bool            hasExtendedLength(const ssa::SSAFunction& fn) const;
};

// ── JSON ──────────────────────────────────────────────────────────────────────

/**
 * @brief JSON detector (generic + library-specific fingerprints).
 *
 * Two-stage analysis:
 *   1. Detect any JSON parser (generic recursive-descent pattern).
 *   2. Classify library: RapidJSON, nlohmann, simdjson, cJSON, or generic.
 *
 * Generic detection:
 *   - `parse_value` function that switches on the first non-whitespace byte.
 *   - Mutual recursion with `parse_object` and `parse_array`.
 *   - String-escape handling (backslash sequences).
 *
 * Library fingerprints:
 *   - **RapidJSON**: template class `GenericDocument`, `kObjectType` constant,
 *     `Value::GetType()` return compared against enum.
 *   - **nlohmann**: template metaprogramming patterns, `basic_json::parse`,
 *     `detail::json_sax_dom_parser` type instantiation.
 *   - **simdjson**: SIMD register loads with `{'\"', '[', '{', ']', '}', ':',
 *     ','}` masks; bit-manipulation for structural-character bitmap.
 *   - **cJSON**: struct `{next, prev, child, type, valuestring, valueint,
 *     valuedouble}` at fixed offsets; `cJSON_Parse` export.
 */
class JSONDetector : public ISerialDetector {
public:
    SerialResult    detect(const ssa::SSAFunction& fn,
                           const std::unordered_set<std::string>& sym) const override;
    SerialFramework framework() const noexcept override { return SerialFramework::JSON; }

private:
    JsonParserEvidence  detectGeneric(const ssa::SSAFunction& fn) const;
    JsonLibraryEvidence detectLibrary(const ssa::SSAFunction& fn,
                                       const std::unordered_set<std::string>& sym) const;

    bool hasRapidJsonSymbols(const std::unordered_set<std::string>& sym) const;
    bool hasNlohmannSymbols(const std::unordered_set<std::string>& sym) const;
    bool hasSimdjsonSimd(const ssa::SSAFunction& fn) const;
    bool hasCJsonSymbols(const std::unordered_set<std::string>& sym) const;
    bool hasValueSwitch(const ssa::SSAFunction& fn) const;
    bool hasMutualRecursion(const ssa::SSAFunction& fn) const;
};

// ── XML ───────────────────────────────────────────────────────────────────────

/**
 * @brief XML detector (generic + library fingerprints).
 *
 * Generic detection:
 *   - `<` / `>` character comparisons for token boundary scanning.
 *   - A tag-name stack (push at element open, pop at element close).
 *   - Attribute key-value parsing (name `=` value pattern).
 *
 * Library-specific fingerprints:
 *   - **libxml2**: `_xmlNode` struct layout (type/name/children/parent/next/prev
 *     pointers at fixed offsets); `xmlParseFile`, `xmlDocGetRootElement`.
 *   - **Expat**: `XML_ParserCreate` + function-pointer registration pattern
 *     (`XML_SetElementHandler`, `XML_SetCharacterDataHandler`).
 *   - **TinyXML-2**: `XMLDocument::Parse` virtual dispatch chain;
 *     `XMLElement::FirstChild` / `NextSibling` accessor pattern.
 *   - **RapidXML**: single-pass in-situ parse with char-pointer output
 *     parameter; template instantiation with char type.
 */
class XMLDetector : public ISerialDetector {
public:
    SerialResult    detect(const ssa::SSAFunction& fn,
                           const std::unordered_set<std::string>& sym) const override;
    SerialFramework framework() const noexcept override { return SerialFramework::XML; }

private:
    XmlParserEvidence detectGeneric(const ssa::SSAFunction& fn) const;
    SerialLibrary     detectLibrary(const ssa::SSAFunction& fn,
                                     const std::unordered_set<std::string>& sym) const;

    bool hasLibxml2Symbols(const std::unordered_set<std::string>& sym) const;
    bool hasExpatSymbols(const std::unordered_set<std::string>& sym) const;
    bool hasTinyXmlSymbols(const std::unordered_set<std::string>& sym) const;
    bool hasRapidXmlSymbols(const std::unordered_set<std::string>& sym) const;
    bool hasTagScanning(const ssa::SSAFunction& fn) const;
    bool hasSaxCallbacks(const ssa::SSAFunction& fn) const;
};

// ─── Schema emitters ──────────────────────────────────────────────────────────

/**
 * @brief Emits `.proto` text from a reconstructed ProtoSchema.
 *
 * Output follows proto3 syntax by default unless the schema specifies proto2.
 *
 * ## Example output
 *
 *   syntax = "proto3";
 *   package my_package;
 *
 *   message Person {
 *     string name    = 1;
 *     int32  age     = 2;
 *     bytes  payload = 3;
 *   }
 */
class ProtoEmitter {
public:
    /// Width of indentation (spaces) per nesting level.
    int indentWidth = 2;

    std::string emit(const ProtoSchema& schema) const;
    std::string emitMessage(const ProtoMessage& msg, int indent = 0) const;
    std::string emitField(const ProtoField& field, int indent = 0) const;
};

/**
 * @brief Emits `.fbs` text from a reconstructed FbsSchema.
 *
 * ## Example output
 *
 *   table Monster {
 *     pos:Vec3;
 *     mana:short = 150;
 *     name:string;
 *     friendly:bool = false;
 *   }
 *   root_type Monster;
 */
class FbsEmitter {
public:
    int indentWidth = 2;

    std::string emit(const FbsSchema& schema) const;
    std::string emitTable(const FbsTable& table, int indent = 0) const;
    std::string emitField(const FbsField& field, int indent = 0) const;
};

// ─── Top-level detector orchestrator ─────────────────────────────────────────

/**
 * @brief Runs all serialisation detectors across a binary module.
 *
 * ## Algorithm
 *
 *   For each function:
 *     1. Quick pre-filter: skip functions with < 3 blocks.
 *     2. Run all detectors (or only enabled ones) in parallel.
 *     3. Keep results with confidence ≥ `minConfidence`.
 *     4. For protobuf / FlatBuffers: group functions by inferred class and
 *        reconstruct schemas from the group.
 *
 * ## Output
 *
 *   `DetectionMap` — maps function name → best SerialResult.
 *   `schemaFiles()` — map of filename → content for emitted schemas.
 */
class SerialDetector {
public:
    struct Config {
        float minConfidence  = 0.40f;
        int   minBlocks      = 3;
        bool  detectProtobuf = true;
        bool  detectFlatBuf  = true;
        bool  detectMsgpack  = true;
        bool  detectCbor     = true;
        bool  detectJson     = true;
        bool  detectXml      = true;
        bool  emitProto      = true;  ///< emit .proto text in results
        bool  emitFbs        = true;  ///< emit .fbs text in results
    };
    static Config defaultConfig() noexcept { return {}; }

    struct Stats {
        uint32_t functionsAnalysed = 0;
        uint32_t functionsSkipped  = 0;
        uint32_t detections        = 0;
        uint32_t protobufClasses   = 0;
        uint32_t flatbufTables     = 0;
        std::unordered_map<SerialFramework, uint32_t> byFramework;
    };

    using DetectionMap = std::unordered_map<std::string, SerialResult>;

    explicit SerialDetector(Config cfg = defaultConfig());

    /**
     * @brief Analyse a single function for any serialisation framework.
     */
    SerialResult analyseFunction(
        const ssa::SSAFunction& fn,
        const std::unordered_set<std::string>& symTable = {}) const;

    /**
     * @brief Analyse an entire module.
     *
     * @param functions  All SSA functions in the binary.
     * @param symTable   All exported / imported symbol names.
     */
    DetectionMap analyseModule(
        const std::vector<const ssa::SSAFunction*>& functions,
        const std::unordered_set<std::string>& symTable = {}) const;

    /**
     * @brief After analyseModule(), returns emitted schema file contents.
     *
     * Keys are suggested filenames (e.g. "recovered.proto", "schema.fbs").
     */
    const std::unordered_map<std::string, std::string>& schemaFiles() const {
        return schemaFiles_;
    }

    const Stats& stats() const { return stats_; }

private:
    Config  cfg_;
    mutable Stats stats_;
    mutable std::unordered_map<std::string, std::string> schemaFiles_;

    std::vector<std::unique_ptr<ISerialDetector>> detectors_;

    ProtoEmitter protoEmitter_;
    FbsEmitter   fbsEmitter_;

    bool passesPreflight(const ssa::SSAFunction& fn) const;

    void groupAndReconstructProtobuf(
        const DetectionMap& detections,
        const std::vector<const ssa::SSAFunction*>& functions,
        const std::unordered_set<std::string>& symTable) const;

    void groupAndReconstructFlatBuffers(
        const DetectionMap& detections,
        const std::vector<const ssa::SSAFunction*>& functions,
        const std::unordered_set<std::string>& symTable) const;
};

// ─── Utility helpers (public for testing) ─────────────────────────────────────

/// Encode a varint into `buf` (up to 10 bytes). Returns bytes written.
int encodeVarint(uint64_t value, uint8_t buf[10]);

/// Decode a varint from `data[0..len)`. Returns {value, bytesRead} or {0,0}.
std::pair<uint64_t, int> decodeVarint(const uint8_t* data, int len);

/// Build a protobuf field tag: (field_number << 3) | wire_type.
uint32_t makeProtoTag(uint32_t fieldNumber, ProtoWireType wt);

/// Decode field_number and wire_type from a tag.
std::pair<uint32_t, ProtoWireType> decodeProtoTag(uint32_t tag);

/// Map a symbol name to its inferred proto field name (strip getters/setters).
std::string symbolToFieldName(const std::string& symbol);

/// Human-readable name for a ProtoScalarType.
const char* protoScalarTypeName(ProtoScalarType t);

/// Human-readable name for a SerialFramework.
const char* serialFrameworkName(SerialFramework f);

/// Human-readable name for a SerialLibrary.
const char* serialLibraryName(SerialLibrary l);

} // namespace serial_detect
} // namespace retdec

#endif // RETDEC_SERIAL_DETECT_H
