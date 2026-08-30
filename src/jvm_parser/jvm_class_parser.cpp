/**
 * @file src/jvm_parser/jvm_class_parser.cpp
 * @brief JVM ClassFile binary parser → BcClass.
 */

#include "retdec/jvm_parser/jvm_class_parser.h"
#include "retdec/jvm_parser/jvm_lifter.h"

#include <map>
#include <memory>
#include <string>
#include <variant>

namespace retdec {
namespace jvm_parser {

using namespace bc_module;
using namespace bc_module::types;

// ─── Access flags → BcAccess ─────────────────────────────────────────────────

static bc_module::BcAccess toAccess(uint16_t flags) {
    bc_module::BcAccess a = bc_module::BcAccess::None;
    if (flags & 0x0001) a = a | bc_module::BcAccess::Public;
    if (flags & 0x0002) a = a | bc_module::BcAccess::Private;
    if (flags & 0x0004) a = a | bc_module::BcAccess::Protected;
    if (flags & 0x0008) a = a | bc_module::BcAccess::Static;
    if (flags & 0x0010) a = a | bc_module::BcAccess::Final;
    if (flags & 0x0020) a = a | bc_module::BcAccess::Synchronized;
    if (flags & 0x0040) {
        a = a | bc_module::BcAccess::Bridge;
        a = a | bc_module::BcAccess::Volatile;
    }
    if (flags & 0x0080) {
        a = a | bc_module::BcAccess::Transient;
        a = a | bc_module::BcAccess::VarArgs;
    }
    if (flags & 0x0100) a = a | bc_module::BcAccess::Native;
    if (flags & 0x0400) a = a | bc_module::BcAccess::Abstract;
    if (flags & 0x0800) a = a | bc_module::BcAccess::Strict;
    if (flags & 0x1000) a = a | bc_module::BcAccess::Synthetic;
    return a;
}

static std::string stripAnnotationType(const std::string& t) {
    if (t.size() >= 2 && t.front() == 'L' && t.back() == ';')
        return t.substr(1, t.size() - 2);
    return t;
}

static BcAnnotationValue toBcValue(const AnnotationValue& av) {
    BcAnnotationValue v;
    if (auto* i = std::get_if<int32_t>(&av.value)) {
        if (av.tag == 'Z') {
            v.kind = BcAnnotationValue::Kind::Bool;
            v.boolValue = *i != 0;
        } else {
            v.kind = BcAnnotationValue::Kind::Int;
            v.intValue = *i;
        }
    } else if (auto* l = std::get_if<int64_t>(&av.value)) {
        v.kind = BcAnnotationValue::Kind::Int;
        v.intValue = *l;
    } else if (auto* f = std::get_if<float>(&av.value)) {
        v.kind = BcAnnotationValue::Kind::Float;
        v.floatValue = *f;
    } else if (auto* d = std::get_if<double>(&av.value)) {
        v.kind = BcAnnotationValue::Kind::Float;
        v.floatValue = *d;
    } else if (auto* s = std::get_if<std::string>(&av.value)) {
        v.kind = BcAnnotationValue::Kind::String;
        v.stringValue = *s;
    } else if (auto* e = std::get_if<EnumConst>(&av.value)) {
        v.kind = BcAnnotationValue::Kind::Enum;
        v.enumTypeName = e->typeName;
        v.enumConstant = e->constName;
    } else if (auto* c = std::get_if<ClassDescStr>(&av.value)) {
        v.kind = BcAnnotationValue::Kind::Type;
        v.stringValue = c->value;
    } else if (auto* a = std::get_if<BcAnnotation>(&av.value)) {
        v.kind = BcAnnotationValue::Kind::Annotation;
        v.stringValue = stripAnnotationType(a->typeName);
    } else if (auto* arr = std::get_if<AnnotationElemArray>(&av.value)) {
        v.kind = BcAnnotationValue::Kind::Array;
        for (const auto& el : arr->values)
            v.arrayValue.push_back(toBcValue(el.elementValue));
    }
    return v;
}

static BcAnnotation toBcAnn(const RawAnnotation& ra, bool visible) {
    BcAnnotation a;
    a.typeName  = stripAnnotationType(ra.typeName);
    a.isVisible = visible;
    for (const auto& e : ra.elements)
        a.elements[e.name] = toBcValue(e.elementValue);
    return a;
}

static void fillAnnotations(std::vector<BcAnnotation>& dest,
                            const std::vector<ParsedAttr>& attrs,
                            const ConstPool& pool) {
    std::vector<ParsedAttr> vis, invis;
    for (const auto& a : attrs) {
        const auto* raw = std::get_if<RawAttr>(&a);
        if (!raw) continue;
        if (raw->name == "RuntimeVisibleAnnotations")
            vis.push_back(a);
        else if (raw->name == "RuntimeInvisibleAnnotations")
            invis.push_back(a);
    }
    for (const auto& ra : getAnnotations(vis, pool))
        dest.push_back(toBcAnn(ra, true));
    for (const auto& ra : getAnnotations(invis, pool))
        dest.push_back(toBcAnn(ra, false));
    bool hasDeprecated = false;
    for (const auto& a : dest)
        if (a.typeName.find("Deprecated") != std::string::npos)
            hasDeprecated = true;
    if (hasDeprecated)
        return;
    for (const auto& a : attrs) {
        const auto* raw = std::get_if<RawAttr>(&a);
        if (!raw || raw->name != "Deprecated")
            continue;
        BcAnnotation d;
        d.typeName  = "java/lang/Deprecated";
        d.isVisible = false;
        dest.push_back(std::move(d));
        return;
    }
}

static void fillParamAnnotations(BcMethod& m,
                                 const std::vector<ParsedAttr>& attrs,
                                 const ConstPool& pool) {
    std::vector<ParsedAttr> vis, invis;
    for (const auto& a : attrs) {
        const auto* raw = std::get_if<RawAttr>(&a);
        if (!raw) continue;
        if (raw->name == "RuntimeVisibleParameterAnnotations")
            vis.push_back(a);
        else if (raw->name == "RuntimeInvisibleParameterAnnotations")
            invis.push_back(a);
    }
    auto rawVis = getParamAnnotations(vis, pool);
    auto rawInv = getParamAnnotations(invis, pool);
    size_t n = rawVis.size();
    if (rawInv.size() > n) n = rawInv.size();
    if (m.paramAnnotations.size() < n)
        m.paramAnnotations.resize(n);
    for (size_t i = 0; i < rawVis.size(); ++i)
        for (const auto& ra : rawVis[i])
            m.paramAnnotations[i].push_back(toBcAnn(ra, true));
    for (size_t i = 0; i < rawInv.size(); ++i)
        for (const auto& ra : rawInv[i])
            m.paramAnnotations[i].push_back(toBcAnn(ra, false));
}

// ─── Field parser ─────────────────────────────────────────────────────────────

static BcField parseField(BinaryReader& r, const ConstPool& pool,
                           const JvmParseOptions& opts, BcClass& cls)
{
    BcField f;
    uint16_t flags   = r.u2();
    uint16_t nameIdx = r.u2();
    uint16_t descIdx = r.u2();
    uint16_t attrCnt = r.u2();

    f.name   = pool.utf8(nameIdx);
    f.type   = JvmSignatureParser::parseDescriptor(pool.utf8(descIdx));
    f.access = toAccess(flags);

    auto attrs = parseAttributes(r, pool, attrCnt, "field");
    f.signature = getSignature(attrs, pool);
    if (!f.signature.empty()) {
        try {
            f.type = JvmSignatureParser::parseFieldSig(f.signature);
        } catch (const JvmParseError&) {
        }
    }
    if (opts.parseAnnotations)
        fillAnnotations(f.annotations, attrs, pool);
    for (const auto& a : attrs) {
        const auto* raw = std::get_if<RawAttr>(&a);
        if (!raw || raw->name != "ConstantValue" || raw->data.size() != 2)
            continue;
        uint16_t idx = (static_cast<uint16_t>(raw->data[0]) << 8) | raw->data[1];
        if (auto* i = std::get_if<CpInt>(&pool.entry(idx)))
            f.constantIntValue = i->value;
        else if (auto* l = std::get_if<CpLong>(&pool.entry(idx)))
            f.constantIntValue = l->value;
        else if (auto* fl = std::get_if<CpFloat>(&pool.entry(idx)))
            f.constantFltValue = fl->value;
        else if (auto* d = std::get_if<CpDouble>(&pool.entry(idx)))
            f.constantFltValue = d->value;
        else if (pool.is(idx, CpTag::String))
            f.constantStrValue = pool.string(idx);
    }
    if (flags & 0x4000)
        cls.enumConstants.push_back(f.name);
    return f;
}

// ─── Method parser ────────────────────────────────────────────────────────────

static BcMethod parseMethod(BinaryReader& r, const ConstPool& pool,
                              const JvmParseOptions& opts,
                              LiftOptions liftOpts = {})
{
    BcMethod m;
    uint16_t flags   = r.u2();
    uint16_t nameIdx = r.u2();
    uint16_t descIdx = r.u2();
    uint16_t attrCnt = r.u2();

    m.name   = pool.utf8(nameIdx);
    m.access = toAccess(flags);
    m.isAbstract = !!(flags & 0x0400);
    m.isNative   = !!(flags & 0x0100);
    std::string desc = pool.utf8(descIdx);
    m.descriptor = JvmSignatureParser::parseMethodDescriptor(desc);
    m.isConstructor = (m.name == "<init>");
    m.isStaticInit  = (m.name == "<clinit>");

    auto attrs = parseAttributes(r, pool, attrCnt, "method");
    if (opts.parseAnnotations) {
        fillAnnotations(m.annotations, attrs, pool);
        fillParamAnnotations(m, attrs, pool);
    }
    m.signature = getSignature(attrs, pool);
    if (!m.signature.empty()) {
        try {
            auto ms = JvmSignatureParser::parseMethodSig(m.signature);
            for (const auto& tp : ms.typeParams)
                m.typeParams.push_back(tp.name);
            m.descriptor.params.clear();
            for (const auto& p : ms.params)
                m.descriptor.params.push_back(std::make_shared<BcType>(p));
            m.descriptor.returnType = std::make_shared<BcType>(ms.returnType);
        } catch (const JvmParseError&) {
        }
    }

    // Exceptions attribute.
    for (const auto& a : attrs) {
        const auto* raw = std::get_if<RawAttr>(&a);
        if (!raw || raw->name != "Exceptions" || raw->data.empty())
            continue;
        const auto& d = raw->data;
        size_t i = 0;
        while (i < d.size()) {
            size_t j = i;
            while (j < d.size() && d[j] != 0)
                ++j;
            m.throwsList.emplace_back(
                reinterpret_cast<const char*>(d.data() + i), j - i);
            i = j + (j < d.size() ? 1 : 0);
            if (j >= d.size())
                break;
        }
    }
    if (m.throwsList.empty() && !m.signature.empty()) {
        try {
            auto ms = JvmSignatureParser::parseMethodSig(m.signature);
            for (const auto& t : ms.throwsTypes) {
                if (t.isRef() && !t.ref().className.empty())
                    m.throwsList.push_back(t.ref().className);
                else {
                    std::string s = t.toString();
                    if (!s.empty() && s != "?")
                        m.throwsList.push_back(std::move(s));
                }
            }
        } catch (const JvmParseError&) {
        }
    }

    // MethodParameters.
    if (const auto* mp = getMethodParameters(attrs)) {
        for (const auto& p : mp->params) m.paramNames.push_back(p.name);
    }

    // AnnotationDefault — stash the default as a synthetic annotation.
    for (const auto& a : attrs) {
        const auto* raw = std::get_if<RawAttr>(&a);
        if (!raw || raw->name != "AnnotationDefault" || raw->data.size() < 3)
            continue;
        char tag = static_cast<char>(raw->data[0]);
        uint16_t idx = (static_cast<uint16_t>(raw->data[1]) << 8)
                     | static_cast<uint16_t>(raw->data[2]);
        BcAnnotation def;
        def.typeName = "AnnotationDefault";
        BcAnnotationValue val;
        if (tag == 's') {
            val.kind = BcAnnotationValue::Kind::String;
            val.stringValue = pool.utf8(idx);
        } else if (tag == 'Z') {
            val.kind = BcAnnotationValue::Kind::Bool;
            if (const auto* cp = std::get_if<CpInt>(&pool.entry(idx)))
                val.boolValue = cp->value != 0;
        } else if (tag == 'B' || tag == 'C' || tag == 'I' || tag == 'S') {
            val.kind = BcAnnotationValue::Kind::Int;
            if (const auto* cp = std::get_if<CpInt>(&pool.entry(idx)))
                val.intValue = cp->value;
        } else if (tag == 'J') {
            val.kind = BcAnnotationValue::Kind::Int;
            if (const auto* cp = std::get_if<CpLong>(&pool.entry(idx)))
                val.intValue = cp->value;
        } else if (tag == 'F') {
            val.kind = BcAnnotationValue::Kind::Float;
            if (const auto* cp = std::get_if<CpFloat>(&pool.entry(idx)))
                val.floatValue = cp->value;
        } else if (tag == 'D') {
            val.kind = BcAnnotationValue::Kind::Float;
            if (const auto* cp = std::get_if<CpDouble>(&pool.entry(idx)))
                val.floatValue = cp->value;
        } else {
            continue;
        }
        def.elements["value"] = std::move(val);
        m.annotations.push_back(std::move(def));
        break;
    }

    // Code attribute.
    if (opts.parseBytecode) {
        if (const auto* code = getCode(attrs)) {
            m.maxStack  = code->maxStack;
            m.maxLocals = code->maxLocals;

            // Lift bytecode → BcCFG.
            JvmLifter lifter(pool, liftOpts);
            auto lift = lifter.lift(*code, desc);
            if (lift.ok) {
                m.cfg = std::move(lift.cfg);
            }

            // Local variable table.
            if (opts.parseDebugInfo) {
                for (const auto& lv : code->localVarTable) {
                    BcLocalVar v;
                    v.index       = lv.index;
                    v.name        = pool.utf8(lv.nameIndex);
                    v.type        = JvmSignatureParser::parseDescriptor(
                                        pool.utf8(lv.descOrSigIndex));
                    v.startOffset = lv.startPc;
                    v.endOffset   = lv.startPc + lv.length;
                    v.isParam     = (lv.startPc == 0);
                    m.locals.push_back(std::move(v));
                }
                for (const auto& lv : code->localVarTypeTable) {
                    for (auto& v : m.locals) {
                        if (v.index != lv.index || v.startOffset != lv.startPc)
                            continue;
                        try {
                            v.type = JvmSignatureParser::parseFieldSig(
                                pool.utf8(lv.descOrSigIndex));
                        } catch (const JvmParseError&) {
                        }
                        break;
                    }
                }
            }
        }
    }

    return m;
}

// ─── parseClassFile ───────────────────────────────────────────────────────────

JvmParseResult parseClassFile(const uint8_t* data, size_t size,
                               const JvmParseOptions& opts)
{
    JvmParseResult res;
    try {
        BinaryReader r(data, size);

        // Magic number.
        uint32_t magic = r.u4();
        if (magic != 0xCAFEBABE)
            throw JvmParseError("invalid class file magic: " +
                                std::to_string(magic));

        res.minorVersion = r.u2();
        res.majorVersion = r.u2();

        if (opts.strictVersion && res.majorVersion > 65)
            throw JvmParseError("unsupported class file version " +
                                std::to_string(res.majorVersion));

        // Constant pool.
        res.pool = ConstPool::read(r); // reads cp_count from stream internally

        // Class access flags.
        uint16_t accessFlags = r.u2();
        res.cls.access = toAccess(accessFlags);
        res.cls.isInterface  = !!(accessFlags & 0x0200);
        res.cls.isAbstract   = !!(accessFlags & 0x0400);
        res.cls.isEnum       = !!(accessFlags & 0x4000);
        res.cls.isAnnotation = !!(accessFlags & 0x2000);
        res.cls.isModule     = !!(accessFlags & 0x8000);

        // This class, super class.
        uint16_t thisClassIdx  = r.u2();
        uint16_t superClassIdx = r.u2();

        res.cls.fqName      = res.pool.className(thisClassIdx);
        auto lastSlash      = res.cls.fqName.rfind('/');
        res.cls.name        = (lastSlash == std::string::npos)
                              ? res.cls.fqName
                              : res.cls.fqName.substr(lastSlash + 1);
        res.cls.packageName = (lastSlash == std::string::npos)
                              ? ""
                              : res.cls.fqName.substr(0, lastSlash);

        if (superClassIdx != 0) {
            std::string superName = res.pool.className(superClassIdx);
            res.cls.superClass   = bc_module::types::Class(superName);
        }

        // Interfaces.
        uint16_t ifaceCount = r.u2();
        for (uint16_t i = 0; i < ifaceCount; ++i) {
            uint16_t idx = r.u2();
            res.cls.interfaces.push_back(
                bc_module::types::Class(res.pool.className(idx)));
        }

        // Fields.
        uint16_t fieldCount = r.u2();
        for (uint16_t i = 0; i < fieldCount; ++i)
            res.cls.fields.push_back(parseField(r, res.pool, opts, res.cls));

        // Methods.
        uint16_t methodCount = r.u2();
        LiftOptions liftOpts;
        liftOpts.mapLineNumbers = opts.parseDebugInfo;
        for (uint16_t i = 0; i < methodCount; ++i)
            res.cls.methods.push_back(parseMethod(r, res.pool, opts, liftOpts));

        // Class-level attributes.
        uint16_t attrCount = r.u2();
        auto classAttrs = parseAttributes(r, res.pool, attrCount, "class");

        // InnerClasses.
        if (const auto* ic = getInnerClasses(classAttrs)) {
            for (const auto& e : ic->classes) {
                if (e.innerClassInfo != 0 && e.outerClassInfo != 0 &&
                    res.pool.className(e.innerClassInfo) == res.cls.fqName)
                {
                    res.cls.outerClass = res.pool.className(e.outerClassInfo);
                }
            }
        }

        // BootstrapMethods.
        if (const auto* bsm = getBootstrap(classAttrs)) {
            res.bootstrap = *bsm;
        }

        if (opts.parseAnnotations)
            fillAnnotations(res.cls.annotations, classAttrs, res.pool);
        res.cls.signature = getSignature(classAttrs, res.pool);
        res.cls.sourceFile = getSourceFile(classAttrs, res.pool);
        if (const auto* rec = getRecord(classAttrs)) {
            res.cls.isRecord = true;
            for (const auto& rc : rec->components) {
                std::string cname = res.pool.utf8(rc.nameIndex);
                for (auto& f : res.cls.fields) {
                    if (f.name != cname) continue;
                    if (rc.signature && !rc.signature->empty()) {
                        f.signature = *rc.signature;
                        try {
                            f.type = JvmSignatureParser::parseFieldSig(*rc.signature);
                        } catch (const JvmParseError&) {
                        }
                    }
                    for (const auto& ra : rc.annotations)
                        f.annotations.push_back(toBcAnn(ra, true));
                    break;
                }
            }
        }
        if (const auto* enc = getEnclosing(classAttrs)) {
            if (res.cls.outerClass.empty() && enc->classIndex != 0)
                res.cls.outerClass = res.pool.className(enc->classIndex);
        }
        if (const auto* nh = getNestHost(classAttrs)) {
            if (nh->hostClassIndex != 0)
                res.cls.nestHost = res.pool.className(nh->hostClassIndex);
        }
        if (const auto* nm = getNestMembers(classAttrs)) {
            for (uint16_t idx : nm->memberIndices) {
                if (idx != 0)
                    res.cls.nestMembers.push_back(res.pool.className(idx));
            }
        }
        if (const auto* ps = getPermittedSubclasses(classAttrs)) {
            for (uint16_t idx : ps->classIndices) {
                if (idx != 0)
                    res.cls.permittedSubclasses.push_back(
                        res.pool.className(idx));
            }
            if (!res.cls.permittedSubclasses.empty())
                res.cls.access = res.cls.access | BcAccess::Sealed;
        }
        if (!res.cls.signature.empty()) {
            try {
                auto cs = JvmSignatureParser::parseClassSig(res.cls.signature);
                for (const auto& tp : cs.typeParams) {
                    std::string s = tp.name;
                    std::string bound = tp.classBound.toString();
                    if (!bound.empty() && bound != "java/lang/Object"
                        && bound != "Object")
                        s += " extends " + bound;
                    res.cls.typeParams.push_back(std::move(s));
                }
            } catch (const JvmParseError&) {
            }
        }

        res.cls.sourceVersion = javaRelease(res.majorVersion);
        res.ok = true;
    } catch (const std::exception& e) {
        res.ok    = false;
        res.error = e.what();
    }
    return res;
}

JvmParseResult parseClassFile(const std::vector<uint8_t>& data,
                               const JvmParseOptions& opts)
{
    return parseClassFile(data.data(), data.size(), opts);
}

// ─── Version helpers ──────────────────────────────────────────────────────────

int javaRelease(uint16_t majorVersion) noexcept {
    if (majorVersion < 45) return 1;
    if (majorVersion == 45) return 1;
    if (majorVersion == 46) return 2;
    if (majorVersion == 47) return 3;
    if (majorVersion == 48) return 4;
    if (majorVersion == 49) return 5;
    if (majorVersion == 50) return 6;
    if (majorVersion == 51) return 7;
    if (majorVersion == 52) return 8;
    if (majorVersion == 53) return 9;
    if (majorVersion == 54) return 10;
    if (majorVersion == 55) return 11;
    if (majorVersion == 56) return 12;
    if (majorVersion == 57) return 13;
    if (majorVersion == 58) return 14;
    if (majorVersion == 59) return 15;
    if (majorVersion == 60) return 16;
    if (majorVersion == 61) return 17;
    if (majorVersion == 62) return 18;
    if (majorVersion == 63) return 19;
    if (majorVersion == 64) return 20;
    if (majorVersion == 65) return 21;
    return static_cast<int>(majorVersion) - 44;
}

std::string javaVersionString(uint16_t major, uint16_t minor) {
    return "Java " + std::to_string(javaRelease(major))
         + " (class version " + std::to_string(major)
         + "." + std::to_string(minor) + ")";
}

} // namespace jvm_parser
} // namespace retdec
