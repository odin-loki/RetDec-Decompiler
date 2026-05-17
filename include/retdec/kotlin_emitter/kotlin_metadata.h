/**
 * @file include/retdec/kotlin_emitter/kotlin_metadata.h
 * @brief Kotlin @kotlin.Metadata annotation detection and deserialization.
 *
 * ## Background
 *
 * Every class compiled by the Kotlin compiler carries a @kotlin.Metadata
 * annotation with these fields:
 *
 *   k  : Int        — class kind (1=Class, 2=File, 3=SyntheticClass,
 *                                 4=MultiFileClassFacade, 5=MultiFileClassPart)
 *   mv : IntArray   — metadata version [major, minor, patch]
 *   bv : IntArray   — bytecode version (deprecated in newer compilers)
 *   d1 : Array<String> — serialized protobuf (ClassProto / PackageProto)
 *   d2 : Array<String> — string table (referenced by proto indices)
 *   xs : String?    — extra string (e.g. module name)
 *   xi : Int        — extra int flags
 *
 * ## Protobuf decoding (self-contained, no dependency on protobuf library)
 *
 * The d1 strings are a Kotlin-specific protobuf binary encoding of class/
 * package metadata.  We implement a minimal protobuf reader sufficient to
 * extract:
 *   - Class kind (regular, data, sealed, object, companion, value, etc.)
 *   - Supertypes with nullability
 *   - Properties (val/var, extension receiver, delegate, suspend)
 *   - Functions (extension receiver, inline, operator, infix, suspend)
 *   - Type parameters with bounds
 *   - Nested/sealed subclass names
 *   - Companion object name
 *
 * Field numbers are taken from the kotlinx-metadata protobuf schema
 * (kotlin/metadata/jvm/src/kotlin/metadata/jvm/deserialization/proto.proto).
 *
 * ## Detection
 *
 * `KotlinMetadataDetector::isKotlin(cls)` checks for the presence of
 * @kotlin.Metadata on a BcClass.  If present, `detect(cls)` decodes the
 * annotation and returns a `KotlinClassMetadata` structure.
 */

#ifndef RETDEC_KOTLIN_EMITTER_KOTLIN_METADATA_H
#define RETDEC_KOTLIN_EMITTER_KOTLIN_METADATA_H

#include <memory>
#include "retdec/bc_module/bc_module.h"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace retdec {
namespace kotlin_emitter {

using namespace bc_module;

// ─── Kotlin class kind ────────────────────────────────────────────────────────

enum class KotlinClassKind : int {
    Class              = 1,
    File               = 2,
    SyntheticClass     = 3,
    MultiFileFacade    = 4,
    MultiFilePart      = 5,
};

// ─── Kotlin class flags (bit-field, from proto ClassFlags) ──────────────────

struct KotlinClassFlags {
    bool isData        = false;  ///< data class
    bool isObject      = false;  ///< object declaration
    bool isCompanion   = false;  ///< companion object
    bool isSealed      = false;  ///< sealed class/interface
    bool isAbstract    = false;
    bool isFinal       = false;
    bool isInner       = false;  ///< inner class (has outer reference)
    bool isInline      = false;  ///< value class (formerly inline)
    bool isInterface   = false;
    bool isEnum        = false;
    bool isAnnotation  = false;
    bool isFunInterface= false;  ///< functional interface (SAM)
    bool isExpect      = false;  ///< multiplatform expect
    bool isExternal    = false;  ///< external declaration
    bool isOpen        = false;  ///< non-final, non-abstract (modality == 1)
    int  visibility    = 0;      ///< 0=local 1=private 2=protected 3=internal 4=public
    int  modality      = 0;      ///< 0=final 1=open 2=abstract 3=sealed
};

// ─── Kotlin type (nullable + generic) ────────────────────────────────────────

struct KotlinTypeArg {
    enum class Variance { In, Out, Inv };
    Variance    variance   = Variance::Inv;
    bool        isStarProj = false; ///< '*' star projection
    // Recursive type (populated after parsing).
    std::shared_ptr<struct KotlinType> type;
};

struct KotlinType {
    std::string className;           ///< FQ class name (from string table)
    bool        nullable = false;    ///< T?
    int         typeParamIdx = -1;   ///< ≥0 if this is a type parameter reference
    std::vector<KotlinTypeArg> typeArgs;

    // For function types: parameters and return type.
    std::vector<std::shared_ptr<KotlinType>> funParams;
    std::shared_ptr<KotlinType>              funReturn;
    bool isFunctionType = false;
    bool isSuspendFunctionType = false;
};

// ─── Kotlin property ─────────────────────────────────────────────────────────

struct KotlinProperty {
    std::string name;
    std::shared_ptr<KotlinType> returnType;
    std::shared_ptr<KotlinType> receiverType; ///< Extension receiver (T.prop)
    bool        isVar       = false;          ///< var (mutable) or val (immutable)
    bool        isLateinit  = false;
    bool        isConst     = false;
    bool        isDelegated = false;          ///< by delegate
    bool        hasGetter   = true;
    bool        hasSetter   = false;
    int         visibility  = 4;              ///< 4=public
    std::vector<std::string> typeParams;
};

// ─── Kotlin function ─────────────────────────────────────────────────────────

struct KotlinValueParam {
    std::string name;
    std::shared_ptr<KotlinType> type;
    bool hasDefault = false;
    bool isCrossInline = false;
    bool isNoInline    = false;
    bool isVarArg      = false;
};

struct KotlinFunction {
    std::string name;
    std::shared_ptr<KotlinType> returnType;
    std::shared_ptr<KotlinType> receiverType; ///< Extension receiver (fun T.foo())
    std::vector<KotlinValueParam> valueParams;
    std::vector<std::string>      typeParams;

    bool isOperator  = false;  ///< operator fun
    bool isInfix     = false;  ///< infix fun
    bool isInline    = false;  ///< inline fun
    bool isTailrec   = false;
    bool isSuspend   = false;  ///< suspend fun (coroutine)
    bool isExternal  = false;
    bool isExpect    = false;
    int  visibility  = 4;
    int  modality    = 0;

    /// JVM method descriptor to correlate with BcMethod.
    std::string jvmDescriptor;
    std::string jvmName;       ///< May differ from Kotlin name (@JvmName)
};

// ─── Type parameter ───────────────────────────────────────────────────────────

struct KotlinTypeParam {
    std::string name;          ///< "T", "E", "K", etc.
    int         id = 0;        ///< Proto index
    std::vector<std::shared_ptr<KotlinType>> upperBounds;
    bool        isReified  = false;  ///< reified type param (inline functions)
    int         variance   = 0;      ///< 0=inv, 1=in, 2=out
};

// ─── Full Kotlin class metadata ───────────────────────────────────────────────

struct KotlinClassMetadata {
    // Raw annotation fields.
    int                  kind = 1;         ///< KotlinClassKind
    std::vector<int>     metadataVersion;  ///< [major, minor, patch]
    std::string          d1;               ///< Decoded proto bytes (d1[0])
    std::vector<std::string> stringTable;  ///< d2 array

    // Decoded class information.
    KotlinClassFlags     flags;
    std::string          name;             ///< Simple class name
    std::string          fqName;           ///< kotlin/... format
    std::string          companionName;    ///< Companion object simple name
    std::string          sourceFile;       ///< From SourceFile annotation

    std::vector<std::shared_ptr<KotlinType>> supertypes;
    std::vector<KotlinTypeParam>             typeParams;
    std::vector<std::string>                 sealedSubclasses;
    std::vector<KotlinProperty>              properties;
    std::vector<KotlinFunction>              functions;
    std::vector<std::string>                 enumEntries;
    std::vector<std::string>                 nestedClasses;

    bool isValid = false;
    std::string error;
};

// ─── Minimal protobuf reader ──────────────────────────────────────────────────

/**
 * @brief Self-contained minimal protobuf decoder sufficient for Kotlin metadata.
 *
 * Implements only the field types used in Kotlin's proto schema:
 *   - VARINT (field type 0): int32, int64, bool, enum, sint32
 *   - LEN   (field type 2): bytes, string, embedded message
 *
 * Does NOT require the protobuf library as a dependency.
 */
class ProtobufReader {
public:
    explicit ProtobufReader(const uint8_t* data, size_t size);
    explicit ProtobufReader(const std::string& bytes);

    struct Field {
        uint32_t number;
        uint8_t  wireType;  ///< 0=varint, 2=len-delimited
        int64_t  varint;
        std::string bytes;  ///< For wire type 2
    };

    bool        atEnd() const;
    bool        readField(Field& out);

    /// Read all fields and return them grouped by field number.
    std::vector<Field> readAll();

private:
    const uint8_t* data_;
    size_t         size_;
    size_t         pos_ = 0;

    bool readVarint(uint64_t& out);
    bool readBytes(std::string& out);
};

// ─── Kotlin metadata detector ─────────────────────────────────────────────────

/**
 * @brief Detects and decodes @kotlin.Metadata on a BcClass.
 *
 * All state is in the returned KotlinClassMetadata; the detector is stateless.
 */
class KotlinMetadataDetector {
public:
    /// Returns true if `cls` was compiled by Kotlin (has @kotlin.Metadata).
    static bool isKotlin(const BcClass& cls);

    /**
     * @brief Decode the @kotlin.Metadata annotation from `cls`.
     *
     * Returns a KotlinClassMetadata with isValid=true on success.
     * On failure, isValid=false and error is set.
     */
    static KotlinClassMetadata detect(const BcClass& cls);

private:
    // Extract the @kotlin.Metadata annotation from cls.
    static const BcAnnotation* findMetadataAnnotation(const BcClass& cls);

    // Extract string-array value from an annotation element.
    static std::vector<std::string>
        extractStringArray(const BcAnnotationValue& val);

    // Extract int-array value from an annotation element.
    static std::vector<int>
        extractIntArray(const BcAnnotationValue& val);

    // Parse the proto bytes in d1[0] into a KotlinClassMetadata.
    static void decodeProto(const std::string& protoBytes,
                             const std::vector<std::string>& stringTable,
                             KotlinClassMetadata& out);

    // Decode a Class proto message.
    static void decodeClassProto(ProtobufReader& reader,
                                  const std::vector<std::string>& strings,
                                  KotlinClassMetadata& out);

    // Decode a single Type proto message into a KotlinType.
    static std::shared_ptr<KotlinType>
        decodeType(ProtobufReader& reader,
                    const std::vector<std::string>& strings);

    // Decode a Property proto message.
    static KotlinProperty
        decodeProperty(ProtobufReader& reader,
                        const std::vector<std::string>& strings);

    // Decode a Function proto message.
    static KotlinFunction
        decodeFunction(ProtobufReader& reader,
                        const std::vector<std::string>& strings);

    // Decode a TypeParameter proto message.
    static KotlinTypeParam
        decodeTypeParam(ProtobufReader& reader,
                         const std::vector<std::string>& strings);

    // Decode class flags from a varint bitmap.
    static KotlinClassFlags decodeClassFlags(int64_t flags);

    // Lookup a name in the string table (handles negative indices for well-known names).
    static std::string lookupString(const std::vector<std::string>& strings,
                                     int32_t idx);
};

} // namespace kotlin_emitter
} // namespace retdec

#endif // RETDEC_KOTLIN_EMITTER_KOTLIN_METADATA_H
