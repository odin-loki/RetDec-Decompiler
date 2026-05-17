/**
 * @file tests/kotlin_emitter/kotlin_emitter_test.cpp
 * @brief Unit tests for the Kotlin emitter pipeline.
 *
 * Coverage:
 *   - ProtobufReader: varint, length-delimited, multi-field
 *   - KotlinMetadataDetector: annotation detection, isKotlin, proto decoding
 *   - KtTypeRenderer: primitives, nullable, arrays, generics, function types
 *   - KtClassReconstructor: data class, sealed, object, extension, suspend,
 *                            value class, companion object
 *   - KotlinEmitter: class header, data class, object, enum, sealed,
 *                    function modifiers, property modifiers
 *   - KtImportSet: implicit kotlin.*, same-package, collision handling
 *   - KotlinFileEmitter: package/header, module emission (Kotlin vs Java)
 */

#include <memory>
#include "retdec/kotlin_emitter/kotlin_metadata.h"
#include "retdec/kotlin_emitter/kotlin_type_system.h"
#include "retdec/kotlin_emitter/kotlin_emitter.h"
#include "retdec/kotlin_emitter/kotlin_file_emitter.h"

#include <gtest/gtest.h>
#include <string>
#include <vector>

using namespace retdec::kotlin_emitter;
using namespace retdec::bc_module;

// ─── Helpers ──────────────────────────────────────────────────────────────────

static BcClass makeClass(const std::string& name,
                          const std::string& fqName = "") {
    BcClass cls;
    cls.name   = name;
    cls.fqName = fqName.empty() ? name : fqName;
    return cls;
}

// Build a minimal @kotlin.Metadata annotation.
static BcAnnotation makeKotlinMetadata(int kind,
                                        const std::string& d1 = "",
                                        const std::vector<std::string>& d2 = {}) {
    BcAnnotation ann;
    ann.typeName = "kotlin/Metadata";

    // k
    BcAnnotationValue kVal;
    kVal.kind     = BcAnnotationValue::Kind::Int;
    kVal.intValue = kind;
    ann.elements["k"] = kVal;

    // mv
    BcAnnotationValue mvVal;
    mvVal.kind = BcAnnotationValue::Kind::Array;
    for (int v : {1, 8, 0}) {
        BcAnnotationValue elem;
        elem.kind     = BcAnnotationValue::Kind::Int;
        elem.intValue = v;
        mvVal.arrayValue.push_back(elem);
    }
    ann.elements["mv"] = mvVal;

    // d1
    if (!d1.empty()) {
        BcAnnotationValue d1Val;
        d1Val.kind        = BcAnnotationValue::Kind::String;
        d1Val.stringValue = d1;
        ann.elements["d1"] = d1Val;
    }

    // d2
    if (!d2.empty()) {
        BcAnnotationValue d2Val;
        d2Val.kind = BcAnnotationValue::Kind::Array;
        for (const auto& s : d2) {
            BcAnnotationValue elem;
            elem.kind        = BcAnnotationValue::Kind::String;
            elem.stringValue = s;
            d2Val.arrayValue.push_back(elem);
        }
        ann.elements["d2"] = d2Val;
    }

    return ann;
}

// Encode a protobuf varint into a string.
static std::string encodeVarint(uint64_t value) {
    std::string out;
    do {
        uint8_t b = static_cast<uint8_t>(value & 0x7F);
        value >>= 7;
        if (value) b |= 0x80;
        out.push_back(static_cast<char>(b));
    } while (value);
    return out;
}

// Encode a protobuf field tag (field_num << 3 | wire_type).
static std::string encodeTag(uint32_t fieldNum, uint8_t wireType) {
    return encodeVarint((static_cast<uint64_t>(fieldNum) << 3) | wireType);
}

// Encode a length-delimited field.
static std::string encodeLenField(uint32_t fieldNum, const std::string& bytes) {
    return encodeTag(fieldNum, 2) + encodeVarint(bytes.size()) + bytes;
}

// Encode a varint field.
static std::string encodeVarintField(uint32_t fieldNum, uint64_t value) {
    return encodeTag(fieldNum, 0) + encodeVarint(value);
}

// ─── ProtobufReader tests ─────────────────────────────────────────────────────

TEST(ProtobufReaderTest, EmptyAtEnd) {
    ProtobufReader r("");
    EXPECT_TRUE(r.atEnd());
}

TEST(ProtobufReaderTest, ReadSingleVarintField) {
    // field 1 (class flags), value = 42
    std::string proto = encodeVarintField(1, 42);
    ProtobufReader r(proto);
    EXPECT_FALSE(r.atEnd());

    auto fields = r.readAll();
    ASSERT_EQ(1u, fields.size());
    EXPECT_EQ(1u, fields[0].number);
    EXPECT_EQ(0u, fields[0].wireType);
    EXPECT_EQ(42, fields[0].varint);
}

TEST(ProtobufReaderTest, ReadLenDelimitedField) {
    std::string payload = "hello";
    std::string proto   = encodeLenField(2, payload);
    ProtobufReader r(proto);
    auto fields = r.readAll();
    ASSERT_EQ(1u, fields.size());
    EXPECT_EQ(2u, fields[0].number);
    EXPECT_EQ(2u, fields[0].wireType);
    EXPECT_EQ("hello", fields[0].bytes);
}

TEST(ProtobufReaderTest, ReadMultipleFields) {
    std::string proto = encodeVarintField(1, 10) +
                        encodeVarintField(3, 7)  +
                        encodeVarintField(3, 0);
    ProtobufReader r(proto);
    auto fields = r.readAll();
    ASSERT_EQ(3u, fields.size());
    EXPECT_EQ(1u,  fields[0].number);
    EXPECT_EQ(10,  fields[0].varint);
    EXPECT_EQ(3u,  fields[1].number);
    EXPECT_EQ(7,   fields[1].varint);
    EXPECT_EQ(3u,  fields[2].number);
    EXPECT_EQ(0,   fields[2].varint);
}

TEST(ProtobufReaderTest, EmbeddedMessage) {
    // field 5 contains an embedded message with field 3 (nullable = 1)
    std::string inner = encodeVarintField(3, 1);
    std::string proto = encodeLenField(5, inner);
    ProtobufReader r(proto);
    auto fields = r.readAll();
    ASSERT_EQ(1u, fields.size());
    EXPECT_EQ(5u, fields[0].number);
    EXPECT_EQ(inner, fields[0].bytes);
}

// ─── KotlinMetadataDetector — isKotlin ───────────────────────────────────────

TEST(KotlinMetadataDetectorTest, IsKotlin_NoAnnotation) {
    BcClass cls = makeClass("Foo");
    EXPECT_FALSE(KotlinMetadataDetector::isKotlin(cls));
}

TEST(KotlinMetadataDetectorTest, IsKotlin_WithMetadata) {
    BcClass cls = makeClass("Foo");
    cls.annotations.push_back(makeKotlinMetadata(1));
    EXPECT_TRUE(KotlinMetadataDetector::isKotlin(cls));
}

TEST(KotlinMetadataDetectorTest, IsKotlin_OtherAnnotationOnly) {
    BcClass cls = makeClass("Bar");
    BcAnnotation ann;
    ann.typeName = "java/lang/Deprecated";
    cls.annotations.push_back(ann);
    EXPECT_FALSE(KotlinMetadataDetector::isKotlin(cls));
}

// ─── KotlinMetadataDetector — detect ─────────────────────────────────────────

TEST(KotlinMetadataDetectorTest, DetectKind1_NoProto) {
    BcClass cls = makeClass("MyClass", "com/example/MyClass");
    cls.annotations.push_back(makeKotlinMetadata(1));  // No d1
    auto meta = KotlinMetadataDetector::detect(cls);
    EXPECT_TRUE(meta.isValid);
    EXPECT_EQ(1, meta.kind);
}

TEST(KotlinMetadataDetectorTest, DetectKind2_FileFacade) {
    BcClass cls = makeClass("FooKt", "com/example/FooKt");
    cls.annotations.push_back(makeKotlinMetadata(2));
    auto meta = KotlinMetadataDetector::detect(cls);
    EXPECT_TRUE(meta.isValid);
    EXPECT_EQ(2, meta.kind);
}

TEST(KotlinMetadataDetectorTest, DetectMetadataVersion) {
    BcClass cls = makeClass("Foo");
    cls.annotations.push_back(makeKotlinMetadata(1));
    auto meta = KotlinMetadataDetector::detect(cls);
    EXPECT_EQ(3u, meta.metadataVersion.size());
    EXPECT_EQ(1, meta.metadataVersion[0]);
    EXPECT_EQ(8, meta.metadataVersion[1]);
}

TEST(KotlinMetadataDetectorTest, DetectStringTable) {
    BcClass cls = makeClass("Foo");
    cls.annotations.push_back(makeKotlinMetadata(1, "", {"MyClass", "myProp"}));
    auto meta = KotlinMetadataDetector::detect(cls);
    ASSERT_EQ(2u, meta.stringTable.size());
    EXPECT_EQ("MyClass", meta.stringTable[0]);
    EXPECT_EQ("myProp",  meta.stringTable[1]);
}

TEST(KotlinMetadataDetectorTest, DetectProto_ClassFlags) {
    // Encode a minimal ClassProto with flags indicating a data class.
    // flags field = 1, value = (data=1 << 7 | visibility=5<<1 | ...) simplified
    // Let's use flags = 0b10000000 = 0x80 = 128 → isData bit set at position 7.
    int flagsVal = (1 << 7);  // isData
    std::string proto = encodeVarintField(1, static_cast<uint64_t>(flagsVal));
    BcClass cls = makeClass("Data", "com/example/Data");
    cls.annotations.push_back(makeKotlinMetadata(1, proto, {}));
    auto meta = KotlinMetadataDetector::detect(cls);
    EXPECT_TRUE(meta.isValid);
    EXPECT_TRUE(meta.flags.isData);
}

TEST(KotlinMetadataDetectorTest, DetectProto_CompanionName) {
    // ClassProto field 24 = companion name (string-table index)
    std::vector<std::string> st = {"Companion"};
    std::string proto = encodeVarintField(24, 0);  // index 0 → "Companion"
    BcClass cls = makeClass("Outer", "com/example/Outer");
    cls.annotations.push_back(makeKotlinMetadata(1, proto, st));
    auto meta = KotlinMetadataDetector::detect(cls);
    EXPECT_TRUE(meta.isValid);
    EXPECT_EQ("Companion", meta.companionName);
}

TEST(KotlinMetadataDetectorTest, DetectProto_NestedClass) {
    std::vector<std::string> st = {"Inner"};
    std::string proto = encodeVarintField(16, 0);  // nestedClass = idx 0
    BcClass cls = makeClass("Outer");
    cls.annotations.push_back(makeKotlinMetadata(1, proto, st));
    auto meta = KotlinMetadataDetector::detect(cls);
    ASSERT_EQ(1u, meta.nestedClasses.size());
    EXPECT_EQ("Inner", meta.nestedClasses[0]);
}

// ─── KtTypeRenderer ───────────────────────────────────────────────────────────

TEST(KtTypeRendererTest, PrimitiveIntType) {
    KtTypeRenderer renderer({});
    KotlinType t;
    t.className = "kotlin/Int";
    EXPECT_EQ("Int", renderer.render(t));
}

TEST(KtTypeRendererTest, NullableString) {
    KtTypeRenderer renderer({});
    KotlinType t;
    t.className = "kotlin/String";
    t.nullable  = true;
    EXPECT_EQ("String?", renderer.render(t));
}

TEST(KtTypeRendererTest, JvmObjectToAny) {
    KtTypeRenderer renderer({});
    KotlinType t;
    t.className = "java/lang/Object";
    EXPECT_EQ("Any", renderer.render(t));
}

TEST(KtTypeRendererTest, ListOfString) {
    KtTypeRenderer renderer({});
    KotlinType t;
    t.className = "kotlin/List";
    KotlinTypeArg arg;
    arg.type = std::make_shared<KotlinType>();
    arg.type->className = "kotlin/String";
    t.typeArgs.push_back(arg);
    EXPECT_EQ("List<String>", renderer.render(t));
}

TEST(KtTypeRendererTest, MapOfStringToInt) {
    KtTypeRenderer renderer({});
    KotlinType t;
    t.className = "kotlin/Map";
    KotlinTypeArg k;
    k.type = std::make_shared<KotlinType>();
    k.type->className = "kotlin/String";
    KotlinTypeArg v;
    v.type = std::make_shared<KotlinType>();
    v.type->className = "kotlin/Int";
    t.typeArgs.push_back(k);
    t.typeArgs.push_back(v);
    EXPECT_EQ("Map<String, Int>", renderer.render(t));
}

TEST(KtTypeRendererTest, StarProjection) {
    KtTypeRenderer renderer({});
    KotlinType t;
    t.className = "kotlin/List";
    KotlinTypeArg arg;
    arg.isStarProj = true;
    t.typeArgs.push_back(arg);
    EXPECT_EQ("List<*>", renderer.render(t));
}

TEST(KtTypeRendererTest, FunctionType) {
    KtTypeRenderer renderer({});
    KotlinType t;
    t.isFunctionType = true;
    auto param = std::make_shared<KotlinType>();
    param->className = "kotlin/Int";
    t.funParams.push_back(param);
    t.funReturn = std::make_shared<KotlinType>();
    t.funReturn->className = "kotlin/String";
    EXPECT_EQ("(Int) -> String", renderer.render(t));
}

TEST(KtTypeRendererTest, SuspendFunctionType) {
    KtTypeRenderer renderer({});
    KotlinType t;
    t.isSuspendFunctionType = true;
    t.funReturn = std::make_shared<KotlinType>();
    t.funReturn->className = "kotlin/Unit";
    EXPECT_EQ("suspend () -> Unit", renderer.render(t));
}

TEST(KtTypeRendererTest, InVariance) {
    KtTypeRenderer renderer({});
    KotlinType t;
    t.className = "kotlin/Comparable";
    KotlinTypeArg arg;
    arg.variance = KotlinTypeArg::Variance::In;
    arg.type = std::make_shared<KotlinType>();
    arg.type->className = "kotlin/String";
    t.typeArgs.push_back(arg);
    EXPECT_EQ("Comparable<in String>", renderer.render(t));
}

TEST(KtTypeRendererTest, OutVariance) {
    KtTypeRenderer renderer({});
    KotlinType t;
    t.className = "kotlin/List";
    KotlinTypeArg arg;
    arg.variance = KotlinTypeArg::Variance::Out;
    arg.type = std::make_shared<KotlinType>();
    arg.type->className = "kotlin/Number";
    t.typeArgs.push_back(arg);
    EXPECT_EQ("List<out Number>", renderer.render(t));
}

TEST(KtTypeRendererTest, TypeParamRef) {
    KtTypeRenderer renderer({});
    KotlinType t;
    t.typeParamIdx = 0;
    t.className    = "T";
    EXPECT_EQ("T", renderer.render(t));
}

TEST(KtTypeRendererTest, NullableTypeParam) {
    KtTypeRenderer renderer({});
    KotlinType t;
    t.typeParamIdx = 1;
    t.className    = "E";
    t.nullable     = true;
    EXPECT_EQ("E?", renderer.render(t));
}

// ─── KtClassReconstructor ─────────────────────────────────────────────────────

static KotlinClassMetadata makeMetaForClass(
        const std::string& fqName,
        const KotlinClassFlags& flags = {},
        const std::vector<std::string>& stringTable = {}) {
    KotlinClassMetadata meta;
    meta.fqName      = fqName;
    meta.flags       = flags;
    meta.stringTable = stringTable;
    meta.isValid     = true;
    // Set name from fqName
    auto pos = fqName.rfind('/');
    meta.name = (pos != std::string::npos) ? fqName.substr(pos + 1) : fqName;
    return meta;
}

TEST(KtClassReconstructorTest, RegularClass) {
    BcClass cls = makeClass("MyClass", "com/example/MyClass");
    KotlinClassMetadata meta = makeMetaForClass("com/example/MyClass");
    KtClassReconstructor recon;
    KtClass kt = recon.reconstruct(cls, meta);
    EXPECT_EQ("MyClass", kt.name);
    EXPECT_EQ("com.example", kt.packageName);
    EXPECT_EQ(KtClassKind::Class, kt.kind);
}

TEST(KtClassReconstructorTest, DataClass) {
    BcClass cls = makeClass("Point", "com/example/Point");
    KotlinClassFlags flags;
    flags.isData = true;
    KotlinClassMetadata meta = makeMetaForClass("com/example/Point", flags);

    KotlinProperty propX;
    propX.name       = "x";
    propX.returnType = std::make_shared<KotlinType>();
    propX.returnType->className = "kotlin/Int";
    meta.properties.push_back(propX);

    KotlinProperty propY;
    propY.name       = "y";
    propY.returnType = std::make_shared<KotlinType>();
    propY.returnType->className = "kotlin/Int";
    meta.properties.push_back(propY);

    KtClassReconstructor recon;
    KtClass kt = recon.reconstruct(cls, meta);
    EXPECT_EQ(KtClassKind::DataClass, kt.kind);
    // Primary constructor params extracted from properties
    EXPECT_EQ(2u, kt.primaryCtorParams.size());
    EXPECT_EQ("x", kt.primaryCtorParams[0].name);
    EXPECT_EQ("Int", kt.primaryCtorParams[0].type);
}

TEST(KtClassReconstructorTest, SealedClass) {
    BcClass cls = makeClass("Shape", "com/example/Shape");
    KotlinClassFlags flags;
    flags.isSealed = true;
    flags.modality = 3;
    KotlinClassMetadata meta = makeMetaForClass("com/example/Shape", flags, {"Circle", "Square"});
    meta.sealedSubclasses = {"com/example/Circle", "com/example/Square"};

    KtClassReconstructor recon;
    KtClass kt = recon.reconstruct(cls, meta);
    EXPECT_EQ(KtClassKind::SealedClass, kt.kind);
    EXPECT_EQ(2u, kt.sealedSubclasses.size());
}

TEST(KtClassReconstructorTest, ObjectDeclaration) {
    BcClass cls = makeClass("Singleton", "com/example/Singleton");
    KotlinClassFlags flags;
    flags.isObject = true;
    KotlinClassMetadata meta = makeMetaForClass("com/example/Singleton", flags);

    KtClassReconstructor recon;
    KtClass kt = recon.reconstruct(cls, meta);
    EXPECT_EQ(KtClassKind::ObjectDecl, kt.kind);
}

TEST(KtClassReconstructorTest, CompanionObject) {
    BcClass cls = makeClass("MyClass", "com/example/MyClass");
    KotlinClassMetadata meta = makeMetaForClass("com/example/MyClass");
    meta.companionName = "Companion";

    KtClassReconstructor recon;
    KtClass kt = recon.reconstruct(cls, meta);
    ASSERT_NE(nullptr, kt.companion.get());
    EXPECT_EQ("Companion", kt.companion->name);
    EXPECT_EQ(KtClassKind::CompanionObject, kt.companion->kind);
}

TEST(KtClassReconstructorTest, ValueClass) {
    BcClass cls = makeClass("Color", "com/example/Color");
    KotlinClassFlags flags;
    flags.isInline = true;
    KotlinClassMetadata meta = makeMetaForClass("com/example/Color", flags);

    KotlinProperty prop;
    prop.name       = "argb";
    prop.returnType = std::make_shared<KotlinType>();
    prop.returnType->className = "kotlin/Int";
    meta.properties.push_back(prop);

    KtClassReconstructor recon;
    KtClass kt = recon.reconstruct(cls, meta);
    EXPECT_EQ(KtClassKind::ValueClass, kt.kind);
    EXPECT_EQ(1u, kt.primaryCtorParams.size());
    EXPECT_EQ("argb", kt.primaryCtorParams[0].name);
}

TEST(KtClassReconstructorTest, ExtensionFunction) {
    BcClass cls = makeClass("StringUtils", "com/example/StringUtils");
    KotlinClassMetadata meta = makeMetaForClass("com/example/StringUtils");

    KotlinFunction fn;
    fn.name = "repeat";
    fn.returnType = std::make_shared<KotlinType>();
    fn.returnType->className = "kotlin/String";
    fn.receiverType = std::make_shared<KotlinType>();
    fn.receiverType->className = "kotlin/String";
    KotlinValueParam p;
    p.name = "n";
    p.type = std::make_shared<KotlinType>();
    p.type->className = "kotlin/Int";
    fn.valueParams.push_back(p);
    meta.functions.push_back(fn);

    KtClassReconstructor recon;
    KtClass kt = recon.reconstruct(cls, meta);
    ASSERT_EQ(1u, kt.functions.size());
    EXPECT_EQ("String", kt.functions[0].receiverType);
    EXPECT_EQ("repeat", kt.functions[0].name);
    ASSERT_EQ(1u, kt.functions[0].params.size());
    EXPECT_EQ("n", kt.functions[0].params[0].name);
}

TEST(KtClassReconstructorTest, SuspendFunction) {
    BcClass cls = makeClass("Repo", "com/example/Repo");
    KotlinClassMetadata meta = makeMetaForClass("com/example/Repo");

    KotlinFunction fn;
    fn.name      = "fetchData";
    fn.isSuspend = true;
    fn.returnType = std::make_shared<KotlinType>();
    fn.returnType->className = "kotlin/String";
    // No Continuation param in meta (it's JVM only)
    meta.functions.push_back(fn);

    KtClassReconstructor recon;
    KtClass kt = recon.reconstruct(cls, meta);
    ASSERT_EQ(1u, kt.functions.size());
    EXPECT_TRUE(kt.functions[0].isSuspend);
}

TEST(KtClassReconstructorTest, InlineReifiedFunction) {
    BcClass cls = makeClass("Utils", "com/example/Utils");
    KotlinClassMetadata meta = makeMetaForClass("com/example/Utils");

    KotlinFunction fn;
    fn.name     = "cast";
    fn.isInline = true;
    fn.typeParams.push_back("T");
    fn.returnType = std::make_shared<KotlinType>();
    fn.returnType->typeParamIdx = 0;
    fn.returnType->className   = "T";
    meta.functions.push_back(fn);

    KtClassReconstructor recon;
    KtClass kt = recon.reconstruct(cls, meta);
    ASSERT_EQ(1u, kt.functions.size());
    EXPECT_TRUE(kt.functions[0].isInline);
    ASSERT_EQ(1u, kt.functions[0].typeParams.size());
}

TEST(KtClassReconstructorTest, OperatorFunction) {
    BcClass cls = makeClass("Vec2", "com/example/Vec2");
    KotlinClassMetadata meta = makeMetaForClass("com/example/Vec2");

    KotlinFunction fn;
    fn.name       = "plus";
    fn.isOperator = true;
    fn.returnType = std::make_shared<KotlinType>();
    fn.returnType->className = "com/example/Vec2";
    meta.functions.push_back(fn);

    KtClassReconstructor recon;
    KtClass kt = recon.reconstruct(cls, meta);
    ASSERT_EQ(1u, kt.functions.size());
    EXPECT_TRUE(kt.functions[0].isOperator);
}

TEST(KtClassReconstructorTest, InfixFunction) {
    BcClass cls = makeClass("Range", "com/example/Range");
    KotlinClassMetadata meta = makeMetaForClass("com/example/Range");

    KotlinFunction fn;
    fn.name    = "until";
    fn.isInfix = true;
    fn.returnType = std::make_shared<KotlinType>();
    fn.returnType->className = "com/example/Range";
    meta.functions.push_back(fn);

    KtClassReconstructor recon;
    KtClass kt = recon.reconstruct(cls, meta);
    ASSERT_EQ(1u, kt.functions.size());
    EXPECT_TRUE(kt.functions[0].isInfix);
}

// ─── KotlinEmitter — class header ────────────────────────────────────────────

static KtClass makeKtClass(const std::string& name, KtClassKind kind) {
    KtClass cls;
    cls.name = name;
    cls.kind = kind;
    return cls;
}

TEST(KotlinEmitterTest, EmitRegularClass) {
    KotlinEmitter emitter;
    KtClass cls = makeKtClass("Foo", KtClassKind::Class);
    CodeWriter writer;
    std::unordered_map<std::string, ReconstructResult> recon;
    emitter.emitClass(cls, recon, writer);
    std::string code = writer.str();
    EXPECT_NE(code.find("class Foo"), std::string::npos);
    EXPECT_NE(code.find("{"), std::string::npos);
    EXPECT_NE(code.find("}"), std::string::npos);
}

TEST(KotlinEmitterTest, EmitDataClass) {
    KotlinEmitter emitter;
    KtClass cls = makeKtClass("Point", KtClassKind::DataClass);
    KtProperty x;
    x.name = "x";
    x.type = "Int";
    x.isPrimaryCtorParam = true;
    KtProperty y;
    y.name = "y";
    y.type = "Int";
    y.isPrimaryCtorParam = true;
    cls.primaryCtorParams = {x, y};
    CodeWriter writer;
    std::unordered_map<std::string, ReconstructResult> recon;
    emitter.emitClass(cls, recon, writer);
    std::string code = writer.str();
    EXPECT_NE(code.find("data class Point"), std::string::npos);
    EXPECT_NE(code.find("val x: Int"), std::string::npos);
    EXPECT_NE(code.find("val y: Int"), std::string::npos);
}

TEST(KotlinEmitterTest, EmitObjectDeclaration) {
    KotlinEmitter emitter;
    KtClass cls = makeKtClass("MySingleton", KtClassKind::ObjectDecl);
    CodeWriter writer;
    std::unordered_map<std::string, ReconstructResult> recon;
    emitter.emitClass(cls, recon, writer);
    std::string code = writer.str();
    EXPECT_NE(code.find("object MySingleton"), std::string::npos);
}

TEST(KotlinEmitterTest, EmitInterface) {
    KotlinEmitter emitter;
    KtClass cls = makeKtClass("Clickable", KtClassKind::Interface);
    KtFunction fn;
    fn.name       = "onClick";
    fn.returnType = "Unit";
    fn.isAbstract = true;
    cls.functions.push_back(fn);
    CodeWriter writer;
    std::unordered_map<std::string, ReconstructResult> recon;
    emitter.emitClass(cls, recon, writer);
    std::string code = writer.str();
    EXPECT_NE(code.find("interface Clickable"), std::string::npos);
    EXPECT_NE(code.find("fun onClick()"), std::string::npos);
}

TEST(KotlinEmitterTest, EmitSealedClass) {
    KotlinEmitter emitter;
    KtClass cls = makeKtClass("Shape", KtClassKind::SealedClass);
    cls.sealedSubclasses = {"Circle", "Square"};
    CodeWriter writer;
    std::unordered_map<std::string, ReconstructResult> recon;
    emitter.emitClass(cls, recon, writer);
    std::string code = writer.str();
    EXPECT_NE(code.find("sealed class Shape"), std::string::npos);
    EXPECT_NE(code.find("Circle"), std::string::npos);
    EXPECT_NE(code.find("Square"), std::string::npos);
}

TEST(KotlinEmitterTest, EmitEnumClass) {
    KotlinEmitter emitter;
    KtClass cls = makeKtClass("Color", KtClassKind::Enum);
    cls.enumEntries = {"RED", "GREEN", "BLUE"};
    CodeWriter writer;
    std::unordered_map<std::string, ReconstructResult> recon;
    emitter.emitClass(cls, recon, writer);
    std::string code = writer.str();
    EXPECT_NE(code.find("enum class Color"), std::string::npos);
    EXPECT_NE(code.find("RED"), std::string::npos);
    EXPECT_NE(code.find("GREEN"), std::string::npos);
    EXPECT_NE(code.find("BLUE"), std::string::npos);
}

TEST(KotlinEmitterTest, EmitValueClass) {
    KotlinEmitter emitter;
    KtClass cls = makeKtClass("UInt32", KtClassKind::ValueClass);
    KtProperty p;
    p.name = "value";
    p.type = "Int";
    p.isPrimaryCtorParam = true;
    cls.primaryCtorParams.push_back(p);
    CodeWriter writer;
    std::unordered_map<std::string, ReconstructResult> recon;
    emitter.emitClass(cls, recon, writer);
    std::string code = writer.str();
    EXPECT_NE(code.find("@JvmInline"), std::string::npos);
    EXPECT_NE(code.find("value class UInt32"), std::string::npos);
    EXPECT_NE(code.find("val value: Int"), std::string::npos);
}

TEST(KotlinEmitterTest, EmitGenericClass) {
    KotlinEmitter emitter;
    KtClass cls = makeKtClass("Box", KtClassKind::Class);
    cls.typeParams = {"T"};
    CodeWriter writer;
    std::unordered_map<std::string, ReconstructResult> recon;
    emitter.emitClass(cls, recon, writer);
    std::string code = writer.str();
    EXPECT_NE(code.find("class Box<T>"), std::string::npos);
}

TEST(KotlinEmitterTest, EmitSuspendFunction) {
    KotlinEmitter emitter;
    KtClass cls = makeKtClass("Repo", KtClassKind::Class);
    KtFunction fn;
    fn.name      = "fetchData";
    fn.returnType= "String";
    fn.isSuspend = true;
    cls.functions.push_back(fn);
    CodeWriter writer;
    std::unordered_map<std::string, ReconstructResult> recon;
    emitter.emitClass(cls, recon, writer);
    std::string code = writer.str();
    EXPECT_NE(code.find("suspend fun fetchData"), std::string::npos);
    EXPECT_NE(code.find(": String"), std::string::npos);
}

TEST(KotlinEmitterTest, EmitExtensionFunction) {
    KotlinEmitter emitter;
    KtClass cls = makeKtClass("StringExt", KtClassKind::Class);
    KtFunction fn;
    fn.name         = "capitalized";
    fn.returnType   = "String";
    fn.receiverType = "String";
    cls.functions.push_back(fn);
    CodeWriter writer;
    std::unordered_map<std::string, ReconstructResult> recon;
    emitter.emitClass(cls, recon, writer);
    std::string code = writer.str();
    EXPECT_NE(code.find("fun String.capitalized()"), std::string::npos);
}

TEST(KotlinEmitterTest, EmitOperatorFunction) {
    KotlinEmitter emitter;
    KtClass cls = makeKtClass("Vec", KtClassKind::Class);
    KtFunction fn;
    fn.name       = "plus";
    fn.returnType = "Vec";
    fn.isOperator = true;
    KtParam p;
    p.name = "other";
    p.type = "Vec";
    fn.params.push_back(p);
    cls.functions.push_back(fn);
    CodeWriter writer;
    std::unordered_map<std::string, ReconstructResult> recon;
    emitter.emitClass(cls, recon, writer);
    std::string code = writer.str();
    EXPECT_NE(code.find("operator fun plus"), std::string::npos);
}

TEST(KotlinEmitterTest, EmitInlineReifiedFunction) {
    KotlinEmitter emitter;
    KtClass cls = makeKtClass("Utils", KtClassKind::Class);
    KtFunction fn;
    fn.name       = "cast";
    fn.returnType = "T";
    fn.isInline   = true;
    fn.typeParams = {"reified T"};
    cls.functions.push_back(fn);
    CodeWriter writer;
    std::unordered_map<std::string, ReconstructResult> recon;
    emitter.emitClass(cls, recon, writer);
    std::string code = writer.str();
    EXPECT_NE(code.find("inline fun"), std::string::npos);
    EXPECT_NE(code.find("reified T"), std::string::npos);
}

TEST(KotlinEmitterTest, EmitInternalVisibility) {
    KotlinEmitter emitter;
    KtClass cls = makeKtClass("Helper", KtClassKind::Class);
    cls.visibility = KtVisibility::Internal;
    CodeWriter writer;
    std::unordered_map<std::string, ReconstructResult> recon;
    emitter.emitClass(cls, recon, writer);
    std::string code = writer.str();
    EXPECT_NE(code.find("internal class Helper"), std::string::npos);
}

TEST(KotlinEmitterTest, EmitSuperClass) {
    KotlinEmitter emitter;
    KtClass cls = makeKtClass("Dog", KtClassKind::Class);
    cls.superClass = "Animal";
    CodeWriter writer;
    std::unordered_map<std::string, ReconstructResult> recon;
    emitter.emitClass(cls, recon, writer);
    std::string code = writer.str();
    EXPECT_NE(code.find("class Dog"), std::string::npos);
    EXPECT_NE(code.find("Animal"), std::string::npos);
}

// ─── KtImportSet ─────────────────────────────────────────────────────────────

TEST(KtImportSetTest, KotlinStringIsImplicit) {
    KtImportSet imports("com.example");
    std::string name = imports.require("kotlin.String");
    EXPECT_EQ("String", name);
    EXPECT_TRUE(imports.empty());
}

TEST(KtImportSetTest, JavaLangIsImplicit) {
    KtImportSet imports("com.example");
    std::string name = imports.require("java.lang.Object");
    EXPECT_EQ("Object", name);
    EXPECT_TRUE(imports.empty());
}

TEST(KtImportSetTest, SamePackageIsImplicit) {
    KtImportSet imports("com.example");
    std::string name = imports.require("com.example.MyClass");
    EXPECT_EQ("MyClass", name);
    EXPECT_TRUE(imports.empty());
}

TEST(KtImportSetTest, OtherPackageAddsImport) {
    KtImportSet imports("com.example");
    std::string name = imports.require("com.other.Helper");
    EXPECT_EQ("Helper", name);
    EXPECT_FALSE(imports.empty());
    auto lines = imports.importLines();
    ASSERT_EQ(1u, lines.size());
    EXPECT_EQ("import com.other.Helper", lines[0]);
}

TEST(KtImportSetTest, SlashSeparatedName) {
    KtImportSet imports("");
    std::string name = imports.require("com/example/Foo");
    EXPECT_EQ("Foo", name);
    auto lines = imports.importLines();
    ASSERT_EQ(1u, lines.size());
    EXPECT_EQ("import com.example.Foo", lines[0]);
}

TEST(KtImportSetTest, CollisionResolution) {
    KtImportSet imports("");
    std::string a = imports.require("com.a.Foo");
    std::string b = imports.require("com.b.Foo");
    EXPECT_EQ("Foo", a);
    // Second one conflicts: returns FQ name
    EXPECT_EQ("com.b.Foo", b);
}

TEST(KtImportSetTest, DuplicateNotAdded) {
    KtImportSet imports("");
    imports.require("com.example.Foo");
    imports.require("com.example.Foo");
    auto lines = imports.importLines();
    EXPECT_EQ(1u, lines.size());
}

TEST(KtImportSetTest, SortedAlphabetically) {
    KtImportSet imports("");
    imports.require("com.z.ZClass");
    imports.require("com.a.AClass");
    imports.require("com.m.MClass");
    auto lines = imports.importLines();
    ASSERT_EQ(3u, lines.size());
    EXPECT_LT(lines[0], lines[1]);
    EXPECT_LT(lines[1], lines[2]);
}

// ─── KotlinFileEmitter ────────────────────────────────────────────────────────

TEST(KotlinFileEmitterTest, EmitNonKotlinClass) {
    KotlinFileEmitter emitter;
    BcClass cls = makeClass("JavaClass", "com/example/JavaClass");
    KtFileResult result = emitter.emitClass(cls);
    EXPECT_FALSE(result.success);
    EXPECT_FALSE(result.error.empty());
}

TEST(KotlinFileEmitterTest, EmitKotlinClass_PackageLine) {
    KotlinFileEmitter emitter;
    BcClass cls = makeClass("MyClass", "com/example/MyClass");
    cls.annotations.push_back(makeKotlinMetadata(1));
    KtFileResult result = emitter.emitClass(cls);
    EXPECT_TRUE(result.success);
    EXPECT_NE(result.sourceCode.find("package com.example"), std::string::npos);
}

TEST(KotlinFileEmitterTest, EmitKotlinClass_ClassInCode) {
    KotlinFileEmitter emitter;
    BcClass cls = makeClass("Widget", "com/example/Widget");
    cls.annotations.push_back(makeKotlinMetadata(1));
    KtFileResult result = emitter.emitClass(cls);
    EXPECT_TRUE(result.success);
    EXPECT_NE(result.sourceCode.find("class Widget"), std::string::npos);
}

TEST(KotlinFileEmitterTest, EmitModule_CountsKotlinAndJava) {
    BcModule module("test", SourceLang::Kotlin);

    BcClass kotlinCls = makeClass("KClass", "com/example/KClass");
    kotlinCls.annotations.push_back(makeKotlinMetadata(1));
    module.addClass(kotlinCls);

    BcClass javaCls = makeClass("JClass", "com/example/JClass");
    module.addClass(javaCls);

    KotlinFileEmitter emitter;
    KtModuleResult result = emitter.emitModule(module);
    EXPECT_EQ(1, result.kotlinCount);
    EXPECT_EQ(1, result.javaCount);
    EXPECT_EQ(2u, result.files.size());
}

TEST(KotlinFileEmitterTest, SourceFileName_DefaultsToClassName) {
    KotlinFileEmitter emitter;
    BcClass cls = makeClass("Greeter", "com/example/Greeter");
    cls.annotations.push_back(makeKotlinMetadata(1));
    KtFileResult result = emitter.emitClass(cls);
    EXPECT_TRUE(result.success);
    // Default source file name should be "Greeter.kt"
    EXPECT_EQ("Greeter.kt", result.sourceFileName);
}

TEST(KotlinFileEmitterTest, SourceFileName_FromSourceFile) {
    KotlinFileEmitter emitter;
    BcClass cls = makeClass("Greeter", "com/example/Greeter");
    cls.sourceFile = "Greeting.kt";
    cls.annotations.push_back(makeKotlinMetadata(1));
    KtFileResult result = emitter.emitClass(cls);
    EXPECT_TRUE(result.success);
    EXPECT_EQ("Greeting.kt", result.sourceFileName);
}

TEST(KotlinFileEmitterTest, DefaultPackageClass) {
    KotlinFileEmitter emitter;
    BcClass cls = makeClass("TopLevel", "TopLevel");
    cls.annotations.push_back(makeKotlinMetadata(1));
    KtFileResult result = emitter.emitClass(cls);
    EXPECT_TRUE(result.success);
    // No package line for default package
    EXPECT_EQ(result.sourceCode.find("package "), std::string::npos);
}

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
