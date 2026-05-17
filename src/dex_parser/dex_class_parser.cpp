/**
 * @file src/dex_parser/dex_class_parser.cpp
 * @brief DEX class_def_item → BcClass converter.
 */

#include <memory>
#include "retdec/dex_parser/dex_class_parser.h"
#include "retdec/bc_module/bc_type.h"

#include <sstream>

namespace retdec {
namespace dex_parser {

using namespace bc_module;

DexClassParser::DexClassParser(const DexFile& dexFile, DexParseOptions opts)
    : dex_(dexFile), opts_(opts), lifter_(dexFile, LiftOptions{opts.parseBytecode}) {}

// ─── Access flag conversion ───────────────────────────────────────────────────

BcAccess DexClassParser::convertAccessFlags(uint32_t flags) const {
    uint32_t acc = 0;
    if (flags & ACC_PUBLIC)       acc |= static_cast<uint32_t>(BcAccess::Public);
    if (flags & ACC_PRIVATE)      acc |= static_cast<uint32_t>(BcAccess::Private);
    if (flags & ACC_PROTECTED)    acc |= static_cast<uint32_t>(BcAccess::Protected);
    if (flags & ACC_STATIC)       acc |= static_cast<uint32_t>(BcAccess::Static);
    if (flags & ACC_FINAL)        acc |= static_cast<uint32_t>(BcAccess::Final);
    if (flags & ACC_ABSTRACT)     acc |= static_cast<uint32_t>(BcAccess::Abstract);
    if (flags & ACC_SYNTHETIC)    acc |= static_cast<uint32_t>(BcAccess::Synthetic);
    if (flags & ACC_NATIVE)       acc |= static_cast<uint32_t>(BcAccess::Native);
    if (flags & ACC_SYNCHRONIZED) acc |= static_cast<uint32_t>(BcAccess::Synchronized);
    return static_cast<BcAccess>(acc);
}

// ─── DEX descriptor → BcType ─────────────────────────────────────────────────

BcType DexClassParser::descriptorToType(const std::string& desc) const {
    if (desc.empty()) return types::Void();
    switch (desc[0]) {
        case 'V': return types::Void();
        case 'Z': return types::Bool();
        case 'B': return types::Byte();
        case 'S': return types::Short();
        case 'C': return types::Char();
        case 'I': return types::Int();
        case 'J': return types::Long();
        case 'F': return types::Float();
        case 'D': return types::Double();
        case '[': {
            std::string elem = desc.substr(1);
            BcRefType ref;
            ref.kind = BcRefKind::Array;
            ref.elementType = std::make_shared<BcType>(descriptorToType(elem));
            return BcType{ref};
        }
        case 'L': {
            // Ldot/class/name; → dot.class.name
            std::string cls = desc.substr(1);
            if (!cls.empty() && cls.back() == ';')
                cls.pop_back();
            for (char& c : cls)
                if (c == '/') c = '.';
            BcRefType ref;
            ref.kind = BcRefKind::Class;
            ref.className = cls;
            return BcType{ref};
        }
        default:
            return types::Void();
    }
}

// ─── Helper: convert DEX class descriptor to dotted name ────────────────────

static std::string descToDotted(const std::string& desc) {
    std::string s = desc;
    if (!s.empty() && s[0] == 'L' && s.back() == ';')
        s = s.substr(1, s.size() - 2);
    for (char& c : s) if (c == '/') c = '.';
    return s;
}

// ─── Field parsing ────────────────────────────────────────────────────────────

void DexClassParser::parseFields(BcClass& cls,
                                  const std::vector<EncodedField>& fields,
                                  bool isStatic) {
    for (const auto& ef : fields) {
        if (ef.fieldIdx >= dex_.fieldCount())
            continue;
        BcField field;
        field.name   = dex_.fieldName(ef.fieldIdx);
        field.type   = descriptorToType(dex_.fieldType(ef.fieldIdx));
        field.access = convertAccessFlags(ef.accessFlags);
        if (isStatic)
            field.access = field.access | BcAccess::Static;
        cls.fields.push_back(std::move(field));
    }
}

// ─── Method parsing ───────────────────────────────────────────────────────────

void DexClassParser::parseMethods(BcClass& cls,
                                   const std::vector<EncodedMethod>& methods) {
    for (const auto& em : methods) {
        if (em.methodIdx >= dex_.methodCount())
            continue;

        BcMethod method;
        method.name   = dex_.methodName(em.methodIdx);
        method.access = convertAccessFlags(em.accessFlags);

        if (method.name == "<init>")  method.isConstructor = true;
        if (method.name == "<clinit>") method.isStaticInit  = true;
        if (em.accessFlags & ACC_ABSTRACT) method.isAbstract = true;
        if (em.accessFlags & ACC_NATIVE)   method.isNative   = true;

        // Build BcFuncType descriptor from proto
        const MethodId& mid   = dex_.methodId(em.methodIdx);
        const ProtoId&  proto = dex_.protoId(mid.protoIdx);

        BcFuncType funcType;
        funcType.returnType = std::make_shared<BcType>(descriptorToType(dex_.typeName(proto.returnTypeIdx)));

        if (proto.parametersOff != 0) {
            auto params = dex_.readTypeList(proto.parametersOff);
            for (uint32_t typeIdx : params) {
                funcType.params.push_back(std::make_shared<BcType>(
                    descriptorToType(dex_.typeName(typeIdx))));
            }
        }
        method.descriptor = std::move(funcType);

        // Lift bytecode if present
        if (opts_.parseBytecode && em.codeOff != 0) {
            try {
                CodeItem code = dex_.readCodeItem(em.codeOff);

                // Populate maxLocals from code item
                method.maxLocals = code.registersSize;

                auto liftResult = lifter_.lift(code, em.methodIdx);
                if (liftResult.status == DexLiftResult::OK)
                    method.cfg = std::move(liftResult.cfg);
            } catch (const std::exception&) {
                // Skip methods with unreadable code
            }
        }

        cls.methods.push_back(std::move(method));
    }
}

// ─── Annotation parsing ───────────────────────────────────────────────────────

void DexClassParser::parseAnnotations(BcClass& cls, uint32_t annotationsOff) {
    if (!opts_.parseAnnotations || annotationsOff == 0)
        return;

    try {
        DexReader r(dex_.rawData(), dex_.rawSize());
        r.seek(annotationsOff);
        uint32_t classAnnotOff = r.u4();
        uint32_t fieldsSize    = r.u4();
        uint32_t methodsSize   = r.u4();
        uint32_t paramsSize    = r.u4();
        r.skip((fieldsSize + methodsSize + paramsSize) * 8u);

        if (classAnnotOff != 0) {
            DexReader ar(dex_.rawData(), dex_.rawSize());
            ar.seek(classAnnotOff);
            uint32_t annSetSize = ar.u4();
            for (uint32_t i = 0; i < annSetSize; ++i) {
                uint32_t annOff = ar.u4();
                if (annOff == 0) continue;

                DexReader br(dex_.rawData(), dex_.rawSize());
                br.seek(annOff);
                br.u1(); // visibility
                uint32_t typeIdx  = br.uleb128();
                uint32_t numElems = br.uleb128();

                if (typeIdx >= dex_.typeCount()) continue;

                BcAnnotation ann;
                ann.typeName = dex_.typeName(typeIdx);

                for (uint32_t e = 0; e < numElems; ++e) {
                    uint32_t nameIdx = br.uleb128();
                    std::string key = (nameIdx < dex_.stringCount())
                                      ? dex_.string(nameIdx) : "";
                    // Skip encoded_value (simplified, just read the type byte)
                    uint8_t valueArg  = br.u1();
                    uint8_t argBits   = (valueArg >> 5) & 0x7;
                    br.skip(static_cast<size_t>(argBits) + 1);

                    BcAnnotationValue val;
                    val.kind = BcAnnotationValue::Kind::String;
                    ann.elements[key] = val;
                }

                cls.annotations.push_back(std::move(ann));
            }
        }
    } catch (const std::exception&) {
        // Non-fatal
    }
}

// ─── Generic signature resolution ────────────────────────────────────────────

std::string DexClassParser::resolveGenericSignature(uint32_t annotationsOff,
                                                      uint32_t memberIdx,
                                                      bool isMethod) const {
    (void)memberIdx; (void)isMethod;
    if (!opts_.resolveGenerics || annotationsOff == 0)
        return {};

    try {
        DexReader r(dex_.rawData(), dex_.rawSize());
        r.seek(annotationsOff);
        uint32_t classAnnotOff = r.u4();
        r.u4(); r.u4(); r.u4();

        if (classAnnotOff == 0) return {};
        DexReader ar(dex_.rawData(), dex_.rawSize());
        ar.seek(classAnnotOff);
        uint32_t annSetSize = ar.u4();
        for (uint32_t i = 0; i < annSetSize; ++i) {
            uint32_t annOff = ar.u4();
            if (annOff == 0) continue;
            DexReader br(dex_.rawData(), dex_.rawSize());
            br.seek(annOff);
            br.u1();
            uint32_t typeIdx = br.uleb128();
            if (typeIdx >= dex_.typeCount()) continue;
            std::string tname = dex_.typeName(typeIdx);
            if (tname != "Ldalvik/annotation/Signature;") continue;

            uint32_t numElems = br.uleb128();
            for (uint32_t e = 0; e < numElems; ++e) {
                br.uleb128(); // name idx
                uint8_t va = br.u1();
                if ((va & 0x1F) != 0x1c) break;
                uint32_t arrSize = br.uleb128();
                std::string sig;
                for (uint32_t j = 0; j < arrSize; ++j) {
                    uint8_t ev = br.u1();
                    uint8_t argBits = (ev >> 5) & 0x7;
                    uint32_t strIdx = 0;
                    for (uint32_t b = 0; b <= argBits; ++b)
                        strIdx |= (static_cast<uint32_t>(br.u1()) << (b * 8));
                    if (strIdx < dex_.stringCount())
                        sig += dex_.string(strIdx);
                }
                return sig;
            }
        }
    } catch (const std::exception&) {}
    return {};
}

// ─── Main class parser ────────────────────────────────────────────────────────

DexClassResult DexClassParser::parseClass(uint32_t classDefIdx) {
    DexClassResult result;

    if (classDefIdx >= dex_.classCount()) {
        result.status = DexClassResult::Error;
        result.error  = "class def index out of range: " + std::to_string(classDefIdx);
        return result;
    }

    const ClassDef& cd = dex_.classDef(classDefIdx);

    auto bcClass = std::make_shared<BcClass>();

    // Class name: "LHello;" → "Hello"
    std::string desc = dex_.typeName(cd.classIdx);
    bcClass->fqName = descToDotted(desc);
    // Simple name (last component)
    std::string dotted = bcClass->fqName;
    size_t lastDot = dotted.rfind('.');
    bcClass->name = (lastDot != std::string::npos) ? dotted.substr(lastDot + 1) : dotted;
    if (lastDot != std::string::npos)
        bcClass->packageName = dotted.substr(0, lastDot);

    // Access flags
    bcClass->access = convertAccessFlags(cd.accessFlags);
    if (cd.accessFlags & ACC_INTERFACE)  bcClass->isInterface  = true;
    if (cd.accessFlags & ACC_ABSTRACT)   bcClass->isAbstract   = true;
    if (cd.accessFlags & ACC_ENUM)       bcClass->isEnum       = true;
    if (cd.accessFlags & ACC_ANNOTATION) bcClass->isAnnotation = true;

    // Superclass
    if (cd.superclassIdx != ClassDef::NO_INDEX) {
        std::string superDesc = dex_.typeName(cd.superclassIdx);
        bcClass->superClass = descriptorToType(superDesc);
    }

    // Interfaces
    if (cd.interfacesOff != 0) {
        auto ifaces = dex_.readTypeList(cd.interfacesOff);
        for (uint32_t ti : ifaces)
            bcClass->interfaces.push_back(descriptorToType(dex_.typeName(ti)));
    }

    // Source file
    if (cd.sourceFileIdx != ClassDef::NO_INDEX && cd.sourceFileIdx < dex_.stringCount())
        bcClass->sourceFile = dex_.string(cd.sourceFileIdx);

    // Annotations
    if (opts_.parseAnnotations)
        parseAnnotations(*bcClass, cd.annotationsOff);

    // Generic signature
    if (opts_.resolveGenerics)
        bcClass->signature = resolveGenericSignature(cd.annotationsOff, cd.classIdx, false);

    // Fields and methods
    if (cd.classDataOff != 0) {
        ClassData classData = dex_.readClassData(cd.classDataOff);
        parseFields(*bcClass, classData.staticFields,   true);
        parseFields(*bcClass, classData.instanceFields, false);
        parseMethods(*bcClass, classData.directMethods);
        parseMethods(*bcClass, classData.virtualMethods);
    }

    result.bcClass = std::move(bcClass);
    return result;
}

} // namespace dex_parser
} // namespace retdec
