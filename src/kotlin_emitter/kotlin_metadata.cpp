/**
 * @file src/kotlin_emitter/kotlin_metadata.cpp
 * @brief Kotlin @kotlin.Metadata annotation detection and protobuf decoding.
 */

#include <memory>
#include "retdec/kotlin_emitter/kotlin_metadata.h"

#include <algorithm>
#include <cassert>
#include <cstring>
#include <stdexcept>

namespace retdec {
namespace kotlin_emitter {

// ─── Protobuf field numbers used in Kotlin's proto schema ────────────────────
// From: kotlin/metadata/jvm/src/kotlin/metadata/jvm/deserialization/proto.proto
// and kotlinx-metadata-jvm sources.

// ClassProto fields
static constexpr uint32_t kClassFlags         = 1;
static constexpr uint32_t kClassFqName        = 3;
static constexpr uint32_t kClassTypeParam     = 4;
static constexpr uint32_t kClassSupertype     = 5;
static constexpr uint32_t kClassConstructor   = 6;
static constexpr uint32_t kClassFunction      = 11;
static constexpr uint32_t kClassProperty      = 12;
static constexpr uint32_t kClassNestedClass   = 16;
static constexpr uint32_t kClassEnumEntry     = 18;
static constexpr uint32_t kClassSealedSubclass= 19;
static constexpr uint32_t kClassCompanion     = 24;

// TypeProto fields
static constexpr uint32_t kTypeClassName      = 1;
static constexpr uint32_t kTypeArg            = 2;
static constexpr uint32_t kTypeNullable       = 3;
static constexpr uint32_t kTypeFlexLower      = 4;
static constexpr uint32_t kTypeTypeParam      = 9;
static constexpr uint32_t kTypeAbbrev         = 10;

// TypeArgProto fields
static constexpr uint32_t kTypeArgVariance    = 1;
static constexpr uint32_t kTypeArgType        = 2;
static constexpr uint32_t kTypeArgStarProj    = 3;

// PropertyProto fields
static constexpr uint32_t kPropFlags          = 1;
static constexpr uint32_t kPropName           = 2;
static constexpr uint32_t kPropReturnType     = 3;
static constexpr uint32_t kPropReceiverType   = 4;

// FunctionProto fields
static constexpr uint32_t kFunFlags           = 1;
static constexpr uint32_t kFunName            = 2;
static constexpr uint32_t kFunReturnType      = 3;
static constexpr uint32_t kFunReceiverType    = 4;
static constexpr uint32_t kFunTypeParam       = 5;
static constexpr uint32_t kFunValueParam      = 6;
static constexpr uint32_t kFunExtJvmDesc      = 100; // JvmMethodSignature in ProtoBuf extension

// TypeParamProto fields
static constexpr uint32_t kTpId              = 1;
static constexpr uint32_t kTpName            = 2;
static constexpr uint32_t kTpIsReified       = 3;
static constexpr uint32_t kTpVariance        = 4;
static constexpr uint32_t kTpUpperBound      = 5;

// ValueParamProto fields
static constexpr uint32_t kVpFlags           = 1;
static constexpr uint32_t kVpName            = 2;
static constexpr uint32_t kVpType            = 3;

// ─── Kotlin predefined class names (negative string-table indices) ────────────
// Kotlin uses a "built-in" table for well-known classes to save space.
// See kotlin.metadata.internal.metadata.deserialization.BuiltInsPackageFragment.
static const char* const kBuiltinNames[] = {
    "",                              // 0 — placeholder
    "kotlin/Any",                    // 1
    "kotlin/Nothing",                // 2
    "kotlin/Unit",                   // 3
    "kotlin/Boolean",                // 4
    "kotlin/Char",                   // 5
    "kotlin/Byte",                   // 6
    "kotlin/Short",                  // 7
    "kotlin/Int",                    // 8
    "kotlin/Long",                   // 9
    "kotlin/Float",                  // 10
    "kotlin/Double",                 // 11
    "kotlin/String",                 // 12
    "kotlin/Enum",                   // 13
    "kotlin/Array",                  // 14
    "kotlin/BooleanArray",           // 15
    "kotlin/CharArray",              // 16
    "kotlin/ByteArray",              // 17
    "kotlin/ShortArray",             // 18
    "kotlin/IntArray",               // 19
    "kotlin/LongArray",              // 20
    "kotlin/FloatArray",             // 21
    "kotlin/DoubleArray",            // 22
    "kotlin/Throwable",              // 23
    "kotlin/Comparable",             // 24
    "kotlin/Iterable",               // 25
    "kotlin/Iterator",               // 26
    "kotlin/Collection",             // 27
    "kotlin/List",                   // 28
    "kotlin/ListIterator",           // 29
    "kotlin/Set",                    // 30
    "kotlin/Map",                    // 31
    "kotlin/Map/Entry",              // 32
    "kotlin/MutableIterable",        // 33
    "kotlin/MutableIterator",        // 34
    "kotlin/MutableCollection",      // 35
    "kotlin/MutableList",            // 36
    "kotlin/MutableListIterator",    // 37
    "kotlin/MutableSet",             // 38
    "kotlin/MutableMap",             // 39
    "kotlin/MutableMap/Entry",       // 40
    "kotlin/Number",                 // 41
    "kotlin/CharSequence",           // 42
    "kotlin/Annotation",             // 43
    "kotlin/Cloneable",              // 44
};
static constexpr int kBuiltinCount = static_cast<int>(
    sizeof(kBuiltinNames) / sizeof(kBuiltinNames[0]));

// ─── ProtobufReader ───────────────────────────────────────────────────────────

ProtobufReader::ProtobufReader(const uint8_t* data, size_t size)
    : data_(data), size_(size), pos_(0) {}

ProtobufReader::ProtobufReader(const std::string& bytes)
    : data_(reinterpret_cast<const uint8_t*>(bytes.data()))
    , size_(bytes.size()), pos_(0) {}

bool ProtobufReader::atEnd() const { return pos_ >= size_; }

bool ProtobufReader::readVarint(uint64_t& out) {
    out = 0;
    int shift = 0;
    while (pos_ < size_) {
        uint8_t b = data_[pos_++];
        out |= static_cast<uint64_t>(b & 0x7F) << shift;
        if (!(b & 0x80)) return true;
        shift += 7;
        if (shift >= 64) return false;
    }
    return false;
}

bool ProtobufReader::readBytes(std::string& out) {
    uint64_t len;
    if (!readVarint(len)) return false;
    if (pos_ + len > size_) return false;
    out.assign(reinterpret_cast<const char*>(data_ + pos_),
               static_cast<size_t>(len));
    pos_ += static_cast<size_t>(len);
    return true;
}

bool ProtobufReader::readField(Field& out) {
    if (atEnd()) return false;
    uint64_t tag;
    if (!readVarint(tag)) return false;
    out.number   = static_cast<uint32_t>(tag >> 3);
    out.wireType = static_cast<uint8_t>(tag & 0x7);

    if (out.wireType == 0) {
        uint64_t v;
        if (!readVarint(v)) return false;
        out.varint = static_cast<int64_t>(v);
    } else if (out.wireType == 2) {
        if (!readBytes(out.bytes)) return false;
        out.varint = 0;
    } else if (out.wireType == 5) {
        // 32-bit (fixed32 / float) — skip
        if (pos_ + 4 > size_) return false;
        pos_ += 4;
        out.varint = 0;
    } else if (out.wireType == 1) {
        // 64-bit (fixed64 / double) — skip
        if (pos_ + 8 > size_) return false;
        pos_ += 8;
        out.varint = 0;
    } else {
        return false;  // Unknown wire type
    }
    return true;
}

std::vector<ProtobufReader::Field> ProtobufReader::readAll() {
    std::vector<Field> fields;
    Field f;
    while (!atEnd() && readField(f))
        fields.push_back(f);
    return fields;
}

// ─── KotlinMetadataDetector ───────────────────────────────────────────────────

bool KotlinMetadataDetector::isKotlin(const BcClass& cls) {
    return findMetadataAnnotation(cls) != nullptr;
}

const BcAnnotation* KotlinMetadataDetector::findMetadataAnnotation(const BcClass& cls) {
    for (const auto& ann : cls.annotations) {
        if (ann.typeName == "kotlin/Metadata" ||
            ann.typeName == "Lkotlin/Metadata;" ||
            ann.typeName == "kotlin.Metadata") {
            return &ann;
        }
    }
    return nullptr;
}

std::vector<std::string>
KotlinMetadataDetector::extractStringArray(const BcAnnotationValue& val) {
    std::vector<std::string> result;
    if (val.kind == BcAnnotationValue::Kind::Array) {
        for (const auto& elem : val.arrayValue) {
            if (elem.kind == BcAnnotationValue::Kind::String)
                result.push_back(elem.stringValue);
        }
    } else if (val.kind == BcAnnotationValue::Kind::String) {
        result.push_back(val.stringValue);
    }
    return result;
}

std::vector<int>
KotlinMetadataDetector::extractIntArray(const BcAnnotationValue& val) {
    std::vector<int> result;
    if (val.kind == BcAnnotationValue::Kind::Array) {
        for (const auto& elem : val.arrayValue) {
            if (elem.kind == BcAnnotationValue::Kind::Int)
                result.push_back(static_cast<int>(elem.intValue));
        }
    } else if (val.kind == BcAnnotationValue::Kind::Int) {
        result.push_back(static_cast<int>(val.intValue));
    }
    return result;
}

KotlinClassMetadata KotlinMetadataDetector::detect(const BcClass& cls) {
    KotlinClassMetadata meta;
    meta.fqName = cls.fqName;
    meta.name   = cls.name;

    // Extract SourceFile attribute.
    meta.sourceFile = cls.sourceFile;

    const BcAnnotation* ann = findMetadataAnnotation(cls);
    if (!ann) {
        meta.error = "No @kotlin.Metadata annotation found";
        return meta;
    }

    // k: class kind
    auto kIt = ann->elements.find("k");
    if (kIt != ann->elements.end() &&
        kIt->second.kind == BcAnnotationValue::Kind::Int)
        meta.kind = static_cast<int>(kIt->second.intValue);
    else
        meta.kind = 1;

    // mv: metadata version
    auto mvIt = ann->elements.find("mv");
    if (mvIt != ann->elements.end())
        meta.metadataVersion = extractIntArray(mvIt->second);

    // d1: proto bytes (array of strings; we use d1[0])
    auto d1It = ann->elements.find("d1");
    if (d1It != ann->elements.end()) {
        auto d1 = extractStringArray(d1It->second);
        if (!d1.empty())
            meta.d1 = d1[0];
    }

    // d2: string table
    auto d2It = ann->elements.find("d2");
    if (d2It != ann->elements.end())
        meta.stringTable = extractStringArray(d2It->second);

    // xs: extra string (module name, etc.)
    // xi: extra int flags — not decoded here

    if (meta.d1.empty()) {
        meta.isValid = true;  // No proto data (e.g. SyntheticClass) — still Kotlin
        return meta;
    }

    // Decode the protobuf.
    try {
        decodeProto(meta.d1, meta.stringTable, meta);
        meta.isValid = true;
    } catch (const std::exception& e) {
        meta.error = e.what();
    }

    return meta;
}

std::string KotlinMetadataDetector::lookupString(
        const std::vector<std::string>& strings, int32_t idx) {
    if (idx < 0) {
        // Negative: built-in class index
        int bi = -idx;
        if (bi < kBuiltinCount)
            return kBuiltinNames[bi];
        return "";
    }
    if (idx < static_cast<int32_t>(strings.size()))
        return strings[static_cast<size_t>(idx)];
    return "";
}

KotlinClassFlags KotlinMetadataDetector::decodeClassFlags(int64_t flags) {
    KotlinClassFlags f;
    // Kotlin proto ClassFlags bit mapping (from protobuf schema):
    //   0: hasAnnotations
    //   1-2: visibility (0=local,1=private,2=private_to_this,3=protected,4=internal,5=public)
    //   3-4: modality (0=final,1=open,2=abstract,3=sealed)
    //   5: isInner
    //   6: isData
    //   7: isExternal
    //   8: isExpect
    //   9: isValue (inline/value class)
    //   10: isFunInterface
    //   11: hasEnumEntries (new in 1.8+)
    // Class kind is encoded separately in the `k` field.
    // isInterface, isEnum, isAnnotation are derived from k or companion flags.
    int vis = static_cast<int>((flags >> 1) & 0x7);
    f.visibility  = vis;
    int mod = static_cast<int>((flags >> 4) & 0x3);
    f.isFinal     = (mod == 0);
    f.isOpen      = (mod == 1);
    f.isAbstract  = (mod == 2);
    f.isSealed    = (mod == 3);
    f.modality    = mod;
    f.isInner     = (flags >> 6) & 1;
    f.isData      = (flags >> 7) & 1;
    f.isExternal  = (flags >> 8) & 1;
    f.isExpect    = (flags >> 9) & 1;
    f.isInline    = (flags >> 10) & 1;  // value class
    f.isFunInterface = (flags >> 11) & 1;
    return f;
}

std::shared_ptr<KotlinType> KotlinMetadataDetector::decodeType(
        ProtobufReader& reader,
        const std::vector<std::string>& strings) {
    auto type = std::make_shared<KotlinType>();
    ProtobufReader::Field f;
    while (reader.readField(f)) {
        if (f.number == kTypeClassName && f.wireType == 0) {
            type->className = lookupString(strings, static_cast<int32_t>(f.varint));
        } else if (f.number == kTypeNullable && f.wireType == 0) {
            type->nullable = (f.varint != 0);
        } else if (f.number == kTypeTypeParam && f.wireType == 0) {
            type->typeParamIdx = static_cast<int>(f.varint);
        } else if (f.number == kTypeArg && f.wireType == 2) {
            KotlinTypeArg arg;
            ProtobufReader argReader(f.bytes);
            ProtobufReader::Field af;
            while (argReader.readField(af)) {
                if (af.number == kTypeArgStarProj && af.wireType == 0) {
                    arg.isStarProj = (af.varint != 0);
                } else if (af.number == kTypeArgVariance && af.wireType == 0) {
                    int var = static_cast<int>(af.varint);
                    if (var == 1)      arg.variance = KotlinTypeArg::Variance::In;
                    else if (var == 2) arg.variance = KotlinTypeArg::Variance::Out;
                    else               arg.variance = KotlinTypeArg::Variance::Inv;
                } else if (af.number == kTypeArgType && af.wireType == 2) {
                    ProtobufReader typeReader(af.bytes);
                    arg.type = decodeType(typeReader, strings);
                }
            }
            type->typeArgs.push_back(std::move(arg));
        }
        // kTypeFlexLower and kTypeAbbrev are skipped for now
    }
    return type;
}

KotlinProperty KotlinMetadataDetector::decodeProperty(
        ProtobufReader& reader,
        const std::vector<std::string>& strings) {
    KotlinProperty prop;
    ProtobufReader::Field f;
    while (reader.readField(f)) {
        if (f.number == kPropFlags && f.wireType == 0) {
            int flags = static_cast<int>(f.varint);
            // Bit layout (from proto PropertyFlags):
            //   0: hasAnnotations
            //   1-3: visibility
            //   4-5: modality
            //   6: isVar
            //   7: hasGetter
            //   8: hasSetter
            //   9: isConst
            //   10: isLateinit
            //   11: hasConstant
            //   12: isExternal
            //   13: isDelegated
            //   14: isExpect
            prop.isVar       = (flags >> 6) & 1;
            prop.hasGetter   = (flags >> 7) & 1;
            prop.hasSetter   = (flags >> 8) & 1;
            prop.isConst     = (flags >> 9) & 1;
            prop.isLateinit  = (flags >> 10) & 1;
            prop.isDelegated = (flags >> 13) & 1;
            prop.visibility  = (flags >> 1) & 0x7;
        } else if (f.number == kPropName && f.wireType == 0) {
            prop.name = lookupString(strings, static_cast<int32_t>(f.varint));
        } else if (f.number == kPropReturnType && f.wireType == 2) {
            ProtobufReader tr(f.bytes);
            prop.returnType = decodeType(tr, strings);
        } else if (f.number == kPropReceiverType && f.wireType == 2) {
            ProtobufReader tr(f.bytes);
            prop.receiverType = decodeType(tr, strings);
        }
    }
    return prop;
}

KotlinFunction KotlinMetadataDetector::decodeFunction(
        ProtobufReader& reader,
        const std::vector<std::string>& strings) {
    KotlinFunction fn;
    ProtobufReader::Field f;
    while (reader.readField(f)) {
        if (f.number == kFunFlags && f.wireType == 0) {
            int flags = static_cast<int>(f.varint);
            // FunctionFlags bit layout:
            //   0: hasAnnotations
            //   1-3: visibility
            //   4-5: modality
            //   6: isOperator
            //   7: isInfix
            //   8: isInline
            //   9: isTailrec
            //   10: isExternal
            //   11: isSuspend
            //   12: isExpect
            fn.isOperator = (flags >> 6) & 1;
            fn.isInfix    = (flags >> 7) & 1;
            fn.isInline   = (flags >> 8) & 1;
            fn.isTailrec  = (flags >> 9) & 1;
            fn.isExternal = (flags >> 10) & 1;
            fn.isSuspend  = (flags >> 11) & 1;
            fn.isExpect   = (flags >> 12) & 1;
            fn.visibility = (flags >> 1) & 0x7;
        } else if (f.number == kFunName && f.wireType == 0) {
            fn.name = lookupString(strings, static_cast<int32_t>(f.varint));
        } else if (f.number == kFunReturnType && f.wireType == 2) {
            ProtobufReader tr(f.bytes);
            fn.returnType = decodeType(tr, strings);
        } else if (f.number == kFunReceiverType && f.wireType == 2) {
            ProtobufReader tr(f.bytes);
            fn.receiverType = decodeType(tr, strings);
        } else if (f.number == kFunValueParam && f.wireType == 2) {
            KotlinValueParam vp;
            ProtobufReader vpr(f.bytes);
            ProtobufReader::Field vf;
            while (vpr.readField(vf)) {
                if (vf.number == kVpName && vf.wireType == 0)
                    vp.name = lookupString(strings, static_cast<int32_t>(vf.varint));
                else if (vf.number == kVpType && vf.wireType == 2) {
                    ProtobufReader tr(vf.bytes);
                    vp.type = decodeType(tr, strings);
                } else if (vf.number == kVpFlags && vf.wireType == 0) {
                    int vflags = static_cast<int>(vf.varint);
                    vp.hasDefault    = (vflags >> 1) & 1;
                    vp.isCrossInline = (vflags >> 2) & 1;
                    vp.isNoInline    = (vflags >> 3) & 1;
                    vp.isVarArg      = (vflags >> 4) & 1;
                }
            }
            fn.valueParams.push_back(std::move(vp));
        } else if (f.number == kFunTypeParam && f.wireType == 2) {
            KotlinTypeParam tp;
            ProtobufReader tpr(f.bytes);
            ProtobufReader::Field tf;
            while (tpr.readField(tf)) {
                if (tf.number == kTpName && tf.wireType == 0)
                    tp.name = lookupString(strings, static_cast<int32_t>(tf.varint));
                else if (tf.number == kTpIsReified && tf.wireType == 0)
                    tp.isReified = (tf.varint != 0);
            }
            fn.typeParams.push_back(tp.name);
        }
    }
    return fn;
}

KotlinTypeParam KotlinMetadataDetector::decodeTypeParam(
        ProtobufReader& reader,
        const std::vector<std::string>& strings) {
    KotlinTypeParam tp;
    ProtobufReader::Field f;
    while (reader.readField(f)) {
        if (f.number == kTpId && f.wireType == 0)
            tp.id = static_cast<int>(f.varint);
        else if (f.number == kTpName && f.wireType == 0)
            tp.name = lookupString(strings, static_cast<int32_t>(f.varint));
        else if (f.number == kTpIsReified && f.wireType == 0)
            tp.isReified = (f.varint != 0);
        else if (f.number == kTpVariance && f.wireType == 0)
            tp.variance = static_cast<int>(f.varint);
        else if (f.number == kTpUpperBound && f.wireType == 2) {
            ProtobufReader tr(f.bytes);
            tp.upperBounds.push_back(decodeType(tr, strings));
        }
    }
    return tp;
}

void KotlinMetadataDetector::decodeClassProto(
        ProtobufReader& reader,
        const std::vector<std::string>& strings,
        KotlinClassMetadata& out) {
    ProtobufReader::Field f;
    while (reader.readField(f)) {
        if (f.number == kClassFlags && f.wireType == 0) {
            out.flags = decodeClassFlags(f.varint);
        } else if (f.number == kClassFqName && f.wireType == 0) {
            out.fqName = lookupString(strings, static_cast<int32_t>(f.varint));
        } else if (f.number == kClassTypeParam && f.wireType == 2) {
            ProtobufReader tr(f.bytes);
            out.typeParams.push_back(decodeTypeParam(tr, strings));
        } else if (f.number == kClassSupertype && f.wireType == 2) {
            ProtobufReader tr(f.bytes);
            out.supertypes.push_back(decodeType(tr, strings));
        } else if (f.number == kClassFunction && f.wireType == 2) {
            ProtobufReader tr(f.bytes);
            out.functions.push_back(decodeFunction(tr, strings));
        } else if (f.number == kClassProperty && f.wireType == 2) {
            ProtobufReader tr(f.bytes);
            out.properties.push_back(decodeProperty(tr, strings));
        } else if (f.number == kClassNestedClass && f.wireType == 0) {
            out.nestedClasses.push_back(
                lookupString(strings, static_cast<int32_t>(f.varint)));
        } else if (f.number == kClassEnumEntry && f.wireType == 2) {
            // EnumEntry has name as field 1
            ProtobufReader er(f.bytes);
            ProtobufReader::Field ef;
            while (er.readField(ef)) {
                if (ef.number == 1 && ef.wireType == 0)
                    out.enumEntries.push_back(
                        lookupString(strings, static_cast<int32_t>(ef.varint)));
            }
        } else if (f.number == kClassSealedSubclass && f.wireType == 0) {
            out.sealedSubclasses.push_back(
                lookupString(strings, static_cast<int32_t>(f.varint)));
        } else if (f.number == kClassCompanion && f.wireType == 0) {
            out.companionName =
                lookupString(strings, static_cast<int32_t>(f.varint));
        }
    }

    // Decode class kind flags into interface/enum/annotation.
    // kind=1 → Class, kind=2 → File, kind=3 → Synthetic, etc.
    if (out.flags.isSealed)    out.flags.isSealed   = true;
    if (out.flags.modality == 3) out.flags.isSealed = true;
}

void KotlinMetadataDetector::decodeProto(
        const std::string& protoBytes,
        const std::vector<std::string>& stringTable,
        KotlinClassMetadata& out) {
    ProtobufReader reader(protoBytes);
    decodeClassProto(reader, stringTable, out);
}

} // namespace kotlin_emitter
} // namespace retdec
