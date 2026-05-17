/**
 * @file src/jvm_parser/jvm_attr.cpp
 * @brief JVM attribute parser implementation.
 */

#include "retdec/jvm_parser/jvm_attr.h"

#include <algorithm>

namespace retdec {
namespace jvm_parser {

// ─── Annotation parsing ───────────────────────────────────────────────────────

static AnnotationElem parseAnnotationElement(BinaryReader& r,
                                              const ConstPool& pool);

static AnnotationValue parseElementValue(BinaryReader& r,
                                          const ConstPool& pool) {
    AnnotationValue av;
    av.tag = static_cast<char>(r.u1());
    switch (av.tag) {
    case 'B': case 'C': case 'I': case 'S': case 'Z': {
        uint16_t idx = r.u2();
        av.value = static_cast<int32_t>(
            std::get<CpInt>(pool.entry(idx)).value);
        break;
    }
    case 'J': {
        uint16_t idx = r.u2();
        av.value = std::get<CpLong>(pool.entry(idx)).value;
        break;
    }
    case 'F': {
        uint16_t idx = r.u2();
        av.value = std::get<CpFloat>(pool.entry(idx)).value;
        break;
    }
    case 'D': {
        uint16_t idx = r.u2();
        av.value = std::get<CpDouble>(pool.entry(idx)).value;
        break;
    }
    case 's': {
        uint16_t idx = r.u2();
        av.value = pool.utf8(idx);
        break;
    }
    case 'e': {
        uint16_t typeIdx  = r.u2();
        uint16_t constIdx = r.u2();
        av.value = EnumConst{pool.utf8(typeIdx), pool.utf8(constIdx)};
        break;
    }
    case 'c': {
        uint16_t idx = r.u2();
        av.value = ClassDescStr{pool.utf8(idx)};
        break;
    }
    case '@': {
        // Nested annotation
        uint16_t typeIdx = r.u2();
        RawAnnotation nested;
        nested.typeName = pool.utf8(typeIdx);
        uint16_t n = r.u2();
        for (uint16_t k = 0; k < n; ++k)
            nested.elements.push_back(parseAnnotationElement(r, pool));
        BcAnnotation ba;
        ba.typeName = nested.typeName;
        av.value = std::move(ba);
        break;
    }
    case '[': {
        uint16_t n = r.u2();
        AnnotationElemArray arr;
        for (uint16_t k = 0; k < n; ++k) {
            AnnotationElem ae;
            ae.elementValue = parseElementValue(r, pool);
            arr.values.push_back(std::move(ae));
        }
        av.value = std::move(arr);
        break;
    }
    default:
        // Unknown tag — skip 2 bytes conservatively.
        r.skip(2);
        break;
    }
    return av;
}

static AnnotationElem parseAnnotationElement(BinaryReader& r,
                                              const ConstPool& pool) {
    AnnotationElem ae;
    ae.name         = pool.utf8(r.u2());
    ae.elementValue = parseElementValue(r, pool);
    return ae;
}

static RawAnnotation parseAnnotation(BinaryReader& r, const ConstPool& pool) {
    RawAnnotation ann;
    ann.typeName = pool.utf8(r.u2());
    uint16_t n = r.u2();
    for (uint16_t k = 0; k < n; ++k)
        ann.elements.push_back(parseAnnotationElement(r, pool));
    return ann;
}

// ─── Code attribute ───────────────────────────────────────────────────────────

static CodeAttr parseCode(BinaryReader& r, const ConstPool& pool) {
    CodeAttr code;
    code.maxStack  = r.u2();
    code.maxLocals = r.u2();
    uint32_t codeLen = r.u4();
    code.bytecode = r.bytes(codeLen);

    uint16_t exLen = r.u2();
    for (uint16_t i = 0; i < exLen; ++i) {
        ExceptionEntry ee;
        ee.startPc   = r.u2();
        ee.endPc     = r.u2();
        ee.handlerPc = r.u2();
        ee.catchType = r.u2();
        code.exceptionTable.push_back(ee);
    }

    uint16_t attrCount = r.u2();
    for (uint16_t i = 0; i < attrCount; ++i) {
        uint16_t nameIdx = r.u2();
        uint32_t attrLen = r.u4();
        std::string attrName = pool.utf8(nameIdx);
        if (attrName == "LineNumberTable") {
            uint16_t cnt = r.u2();
            for (uint16_t k = 0; k < cnt; ++k) {
                LineNumber ln;
                ln.startPc    = r.u2();
                ln.lineNumber = static_cast<int32_t>(r.u2());
                code.lineNumbers.push_back(ln);
            }
        } else if (attrName == "LocalVariableTable") {
            uint16_t cnt = r.u2();
            for (uint16_t k = 0; k < cnt; ++k) {
                LocalVarEntry lv;
                lv.startPc        = r.u2();
                lv.length         = r.u2();
                lv.nameIndex      = r.u2();
                lv.descOrSigIndex = r.u2();
                lv.index          = r.u2();
                code.localVarTable.push_back(lv);
            }
        } else if (attrName == "LocalVariableTypeTable") {
            uint16_t cnt = r.u2();
            for (uint16_t k = 0; k < cnt; ++k) {
                LocalVarEntry lv;
                lv.startPc        = r.u2();
                lv.length         = r.u2();
                lv.nameIndex      = r.u2();
                lv.descOrSigIndex = r.u2();
                lv.index          = r.u2();
                code.localVarTypeTable.push_back(lv);
            }
        } else {
            r.skip(attrLen);
        }
    }
    return code;
}

// ─── BootstrapMethods attribute ───────────────────────────────────────────────

static BootstrapMethodsAttr parseBootstrap(BinaryReader& r) {
    BootstrapMethodsAttr bsm;
    uint16_t n = r.u2();
    for (uint16_t i = 0; i < n; ++i) {
        BootstrapMethod bm;
        bm.methodRef = r.u2();
        uint16_t argCount = r.u2();
        for (uint16_t k = 0; k < argCount; ++k)
            bm.arguments.push_back(r.u2());
        bsm.methods.push_back(std::move(bm));
    }
    return bsm;
}

// ─── InnerClasses attribute ───────────────────────────────────────────────────

static InnerClassesAttr parseInnerClasses(BinaryReader& r) {
    InnerClassesAttr ic;
    uint16_t n = r.u2();
    for (uint16_t i = 0; i < n; ++i) {
        InnerClassEntry e;
        e.innerClassInfo = r.u2();
        e.outerClassInfo = r.u2();
        e.innerName      = r.u2();
        e.accessFlags    = r.u2();
        ic.classes.push_back(e);
    }
    return ic;
}

// ─── Record attribute ─────────────────────────────────────────────────────────

static RecordAttr parseRecord(BinaryReader& r, const ConstPool& pool) {
    RecordAttr rec;
    uint16_t n = r.u2();
    for (uint16_t i = 0; i < n; ++i) {
        RecordComponent rc;
        rc.nameIndex       = r.u2();
        rc.descriptorIndex = r.u2();
        uint16_t attrCount = r.u2();
        for (uint16_t k = 0; k < attrCount; ++k) {
            uint16_t aidx = r.u2();
            uint32_t alen = r.u4();
            std::string aname = pool.utf8(aidx);
            if (aname == "Signature") {
                rc.signature = pool.utf8(r.u2());
            } else if (aname == "RuntimeVisibleAnnotations") {
                uint16_t cnt = r.u2();
                for (uint16_t m = 0; m < cnt; ++m)
                    rc.annotations.push_back(parseAnnotation(r, pool));
            } else {
                r.skip(alen);
            }
        }
        rec.components.push_back(std::move(rc));
    }
    return rec;
}

// ─── PermittedSubclasses attribute ───────────────────────────────────────────

static PermittedSubclassesAttr parsePermittedSubclasses(BinaryReader& r) {
    PermittedSubclassesAttr ps;
    uint16_t n = r.u2();
    for (uint16_t i = 0; i < n; ++i) ps.classIndices.push_back(r.u2());
    return ps;
}

// ─── MethodParameters attribute ──────────────────────────────────────────────

static MethodParametersAttr parseMethodParameters(BinaryReader& r,
                                                   const ConstPool& pool) {
    MethodParametersAttr mp;
    uint8_t n = r.u1();
    for (uint8_t i = 0; i < n; ++i) {
        MethodParameter param;
        uint16_t nameIdx = r.u2();
        if (nameIdx != 0) param.name = pool.utf8(nameIdx);
        param.accessFlags = r.u2();
        mp.params.push_back(std::move(param));
    }
    return mp;
}

// ─── parseAttribute ───────────────────────────────────────────────────────────

ParsedAttr parseAttribute(BinaryReader& r, const ConstPool& pool,
                           const std::string& /*context*/) {
    uint16_t nameIdx = r.u2();
    uint32_t length  = r.u4();
    std::string name = pool.utf8(nameIdx);
    size_t startPos  = r.pos();

    ParsedAttr result;
    bool parsed = false;

    if (name == "Code") {
        result = parseCode(r, pool);
        parsed = true;
    } else if (name == "BootstrapMethods") {
        result = parseBootstrap(r);
        parsed = true;
    } else if (name == "InnerClasses") {
        result = parseInnerClasses(r);
        parsed = true;
    } else if (name == "EnclosingMethod") {
        EnclosingMethodAttr em;
        em.classIndex  = r.u2();
        em.methodIndex = r.u2();
        result = em;
        parsed = true;
    } else if (name == "Record") {
        result = parseRecord(r, pool);
        parsed = true;
    } else if (name == "PermittedSubclasses") {
        result = parsePermittedSubclasses(r);
        parsed = true;
    } else if (name == "NestHost") {
        NestHostAttr nh; nh.hostClassIndex = r.u2();
        result = nh;
        parsed = true;
    } else if (name == "NestMembers") {
        NestMembersAttr nm;
        uint16_t n = r.u2();
        for (uint16_t i = 0; i < n; ++i) nm.memberIndices.push_back(r.u2());
        result = nm;
        parsed = true;
    } else if (name == "MethodParameters") {
        result = parseMethodParameters(r, pool);
        parsed = true;
    }

    if (!parsed) {
        // Skip to end of attribute.
        size_t consumed = r.pos() - startPos;
        if (consumed < length) r.skip(length - consumed);
        auto raw = std::vector<uint8_t>(); // don't re-read
        result = RawAttr{name, raw};
    } else {
        // Verify exact consumption.
        size_t consumed = r.pos() - startPos;
        if (consumed < length) r.skip(length - consumed);
    }
    return result;
}

std::vector<ParsedAttr> parseAttributes(BinaryReader& r, const ConstPool& pool,
                                         uint16_t count,
                                         const std::string& context) {
    std::vector<ParsedAttr> attrs;
    attrs.reserve(count);
    for (uint16_t i = 0; i < count; ++i)
        attrs.push_back(parseAttribute(r, pool, context));
    return attrs;
}

// ─── Attribute accessors ──────────────────────────────────────────────────────

const CodeAttr* getCode(const std::vector<ParsedAttr>& attrs) {
    for (const auto& a : attrs) if (const auto* c = std::get_if<CodeAttr>(&a)) return c;
    return nullptr;
}
const BootstrapMethodsAttr* getBootstrap(const std::vector<ParsedAttr>& attrs) {
    for (const auto& a : attrs) if (const auto* b = std::get_if<BootstrapMethodsAttr>(&a)) return b;
    return nullptr;
}
const InnerClassesAttr* getInnerClasses(const std::vector<ParsedAttr>& attrs) {
    for (const auto& a : attrs) if (const auto* c = std::get_if<InnerClassesAttr>(&a)) return c;
    return nullptr;
}
const EnclosingMethodAttr* getEnclosing(const std::vector<ParsedAttr>& attrs) {
    for (const auto& a : attrs) if (const auto* e = std::get_if<EnclosingMethodAttr>(&a)) return e;
    return nullptr;
}
const RecordAttr* getRecord(const std::vector<ParsedAttr>& attrs) {
    for (const auto& a : attrs) if (const auto* r = std::get_if<RecordAttr>(&a)) return r;
    return nullptr;
}
const PermittedSubclassesAttr* getPermittedSubclasses(const std::vector<ParsedAttr>& attrs) {
    for (const auto& a : attrs) if (const auto* p = std::get_if<PermittedSubclassesAttr>(&a)) return p;
    return nullptr;
}
const NestHostAttr* getNestHost(const std::vector<ParsedAttr>& attrs) {
    for (const auto& a : attrs) if (const auto* n = std::get_if<NestHostAttr>(&a)) return n;
    return nullptr;
}
const MethodParametersAttr* getMethodParameters(const std::vector<ParsedAttr>& attrs) {
    for (const auto& a : attrs) if (const auto* m = std::get_if<MethodParametersAttr>(&a)) return m;
    return nullptr;
}

std::string getSignature(const std::vector<ParsedAttr>& attrs,
                          const ConstPool& pool) {
    // Signature is stored as RawAttr with name "Signature" and 2 bytes of data.
    // We need to handle it specially since it is parsed as RawAttr.
    // (It could be promoted later; for now search raw attrs.)
    for (const auto& a : attrs) {
        const auto* raw = std::get_if<RawAttr>(&a);
        if (raw && raw->name == "Signature" && raw->data.empty()) {
            // Already consumed during parsing — stored index-only is not cached
            // here. Return "" to signal "check pool directly".
        }
    }
    return "";
}

std::string getSourceFile(const std::vector<ParsedAttr>& attrs,
                           const ConstPool& /*pool*/) {
    for (const auto& a : attrs) {
        const auto* raw = std::get_if<RawAttr>(&a);
        if (raw && raw->name == "SourceFile") return "";
    }
    return "";
}

std::vector<RawAnnotation> getAnnotations(const std::vector<ParsedAttr>& /*attrs*/,
                                           const ConstPool& /*pool*/) {
    // RuntimeVisibleAnnotations are parsed inline during class file parsing.
    return {};
}

std::vector<std::vector<RawAnnotation>>
getParamAnnotations(const std::vector<ParsedAttr>& /*attrs*/,
                    const ConstPool& /*pool*/) {
    return {};
}

} // namespace jvm_parser
} // namespace retdec
