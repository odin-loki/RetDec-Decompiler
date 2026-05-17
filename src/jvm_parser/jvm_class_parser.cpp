/**
 * @file src/jvm_parser/jvm_class_parser.cpp
 * @brief JVM ClassFile binary parser → BcClass.
 */

#include "retdec/jvm_parser/jvm_class_parser.h"
#include "retdec/jvm_parser/jvm_lifter.h"

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
    if (flags & 0x0040) a = a | bc_module::BcAccess::Bridge;   // volatile / bridge
    if (flags & 0x0080) a = a | bc_module::BcAccess::Transient; // transient / varargs
    if (flags & 0x0100) a = a | bc_module::BcAccess::Native;
    if (flags & 0x0200) a = a | bc_module::BcAccess::Abstract;
    if (flags & 0x0800) a = a | bc_module::BcAccess::Synthetic;
    return a;
}

// ─── Field parser ─────────────────────────────────────────────────────────────

static BcField parseField(BinaryReader& r, const ConstPool& pool,
                           const JvmParseOptions& opts)
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
    (void)opts;

    // ConstantValue attribute.
    for (const auto& a : attrs) {
        if (const auto* raw = std::get_if<RawAttr>(&a)) {
            if (raw->name == "Signature") {
                // Signature was consumed as RawAttr with empty data; skip.
            }
        }
    }
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
    std::string desc = pool.utf8(descIdx);
    m.descriptor = JvmSignatureParser::parseMethodDescriptor(desc);
    m.isConstructor = (m.name == "<init>");
    m.isStaticInit  = (m.name == "<clinit>");

    auto attrs = parseAttributes(r, pool, attrCnt, "method");

    // Exceptions attribute.
    for (const auto& a : attrs) {
        if (const auto* raw = std::get_if<RawAttr>(&a)) {
            if (raw->name == "Exceptions") {
                // Would parse the exceptions index list here.
            }
        }
    }

    // MethodParameters.
    if (const auto* mp = getMethodParameters(attrs)) {
        for (const auto& p : mp->params) m.paramNames.push_back(p.name);
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
            res.cls.fields.push_back(parseField(r, res.pool, opts));

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

        // SourceFile.
        for (const auto& a : classAttrs) {
            if (const auto* raw = std::get_if<RawAttr>(&a)) {
                if (raw->name == "SourceFile" || raw->name == "Signature") {
                    // Would extract from raw data if we stored it.
                }
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
