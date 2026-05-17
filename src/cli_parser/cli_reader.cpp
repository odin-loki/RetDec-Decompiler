/**
 * @file src/cli_parser/cli_reader.cpp
 * @brief Top-level .NET CLI reader — produces a BcModule from a PE assembly.
 */

#include <memory>
#include "retdec/cli_parser/cli_reader.h"

#include <algorithm>
#include <cassert>
#include <cstring>
#include <fstream>
#include <vector>

namespace retdec {
namespace cli_parser {

// ─── CLIReader ────────────────────────────────────────────────────────────────

CLIReader::CLIReader(const CliReadOptions& opts) : opts_(opts) {}

CliReadResult CLIReader::readFile(const std::string& path) {
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    if (!f.is_open()) {
        CliReadResult r;
        r.error = "Cannot open file: " + path;
        return r;
    }
    auto sz = static_cast<size_t>(f.tellg());
    f.seekg(0);
    std::vector<uint8_t> buf(sz);
    f.read(reinterpret_cast<char*>(buf.data()), sz);
    std::string name = path;
    auto slash = name.find_last_of("/\\");
    if (slash != std::string::npos) name = name.substr(slash + 1);
    return read(buf.data(), sz, name);
}

CliReadResult CLIReader::read(const uint8_t* data, size_t size,
                               const std::string& name) {
    CliReadResult result;

    // Reset state
    fileData_ = data;
    fileSize_ = size;
    typeDefNames_.clear();
    typeRefNames_.clear();
    nestedMap_.clear();

    // Phase 1: PE parsing
    if (!pe_.open(data, size)) {
        result.error = "PE parse failed: " + pe_.error();
        return result;
    }
    if (!pe_.hasCLI()) {
        result.error = "Not a .NET assembly (no CLI directory)";
        return result;
    }

    // Phase 2: Build heap set
    auto tildeData  = pe_.streamBytes("#~");
    auto strData    = pe_.streamBytes("#Strings");
    auto usData     = pe_.streamBytes("#US");
    auto guidData   = pe_.streamBytes("#GUID");
    auto blobData   = pe_.streamBytes("#Blob");

    // HeapSizes comes from the #~ stream header byte 6
    uint8_t heapSizes = (tildeData.size() >= 7) ? tildeData[6] : 0;

    heaps_ = std::make_unique<CliHeaps>(
        strData, usData, guidData, blobData, heapSizes);

    // Phase 3: Metadata tables
    tables_ = std::make_unique<MetadataTables>();
    if (!tables_->parse(tildeData, *heaps_)) {
        result.error = "Metadata table parse failed: " + tables_->error();
        return result;
    }

    // Phase 4: Build type name cache
    if (!buildTypeNames()) {
        result.error = "Failed to build type name cache";
        return result;
    }

    // Phase 5: Build sig decoder + CIL lifter
    sigDecoder_ = std::make_unique<CliSigDecoder>(this);
    if (opts_.decodeCIL)
        cilLifter_ = std::make_unique<CILLifter>(this);

    // Phase 6: Build NestedClass map
    uint32_t numNested = tables_->rowCount(TableId::NestedClass);
    for (uint32_t i = 1; i <= numNested; ++i) {
        auto row = tables_->nestedClass(i);
        nestedMap_.emplace_back(row.nestedClass, row.enclosingClass);
    }

    // Phase 7: Determine assembly name
    std::string asmName = name;
    if (tables_->rowCount(TableId::Assembly) >= 1) {
        auto asmRow = tables_->assembly(1);
        std::string fromMeta = heaps_->strings.get(asmRow.name);
        if (!fromMeta.empty()) asmName = fromMeta;
    }

    // Phase 8: Build BcModule
    BcModule module(asmName, SourceLang::CSharp);
    module.setName(asmName);

    uint32_t numTypeDefs = tables_->rowCount(TableId::TypeDef);
    result.typeDefCount = numTypeDefs;

    // Skip TypeDef index 1 = <Module> (it's a pseudo-class for module-level methods)
    for (uint32_t ti = 2; ti <= numTypeDefs; ++ti) {
        auto row = tables_->typeDef(ti);
        std::string className = typeDefName(ti);

        // Skip compiler-generated types
        if (opts_.skipSynthetic) {
            // Types starting with '<' are compiler generated
            std::string simpleName = heaps_->strings.get(row.name);
            if (!simpleName.empty() && simpleName[0] == '<') continue;
        }

        // Skip private types if requested
        if (opts_.skipPrivate) {
            uint32_t vis = row.flags & TypeAttributes::VisibilityMask;
            if (vis == TypeAttributes::NotPublic ||
                vis == TypeAttributes::NestedPrivate)
                continue;
        }

        ++result.typeDefCount;

        try {
            BcClass cls = buildClass(ti, result);
            module.addClass(std::move(cls));
        } catch (...) {
            ++result.parseErrorCount;
        }
    }

    result.module  = std::move(module);
    result.success = true;
    return result;
}

// ─── ITypeNameResolver ────────────────────────────────────────────────────────

std::string CLIReader::typeDefName(uint32_t idx) const {
    if (idx == 0 || idx > typeDefNames_.size()) return "<unknown>";
    return typeDefNames_[idx - 1];
}

std::string CLIReader::typeRefName(uint32_t idx) const {
    if (idx == 0 || idx > typeRefNames_.size()) return "<unknown>";
    return typeRefNames_[idx - 1];
}

BcType CLIReader::typeSpecType(uint32_t idx) const {
    if (!tables_ || !sigDecoder_) return types::ClrObject();
    uint32_t numSpecs = tables_->rowCount(TableId::TypeSpec);
    if (idx == 0 || idx > numSpecs) return types::ClrObject();
    auto row = tables_->typeSpec(idx);
    auto blob = heaps_->blobs.get(row.signature);
    auto ct = sigDecoder_->decodeTypeSpec(blob);
    if (!ct) return types::ClrObject();
    return ct->base;
}

// ─── buildTypeNames ───────────────────────────────────────────────────────────

bool CLIReader::buildTypeNames() {
    uint32_t numTypeDefs = tables_->rowCount(TableId::TypeDef);
    typeDefNames_.resize(numTypeDefs);

    for (uint32_t i = 1; i <= numTypeDefs; ++i) {
        auto row = tables_->typeDef(i);
        std::string ns   = heaps_->strings.get(row.ns);
        std::string name = heaps_->strings.get(row.name);
        typeDefNames_[i - 1] = makeClrName(ns, name);
    }

    // Fix up nested type names (Outer+Inner)
    for (const auto& [nested, enclosing] : nestedMap_) {
        if (nested == 0 || nested > numTypeDefs) continue;
        if (enclosing == 0 || enclosing > numTypeDefs) continue;
        auto row = tables_->typeDef(nested);
        std::string innerName = heaps_->strings.get(row.name);
        // Get enclosing's FQ name, then append +Inner
        typeDefNames_[nested - 1] = typeDefNames_[enclosing - 1] + "+" + innerName;
    }

    // TypeRef names
    uint32_t numTypeRefs = tables_->rowCount(TableId::TypeRef);
    typeRefNames_.resize(numTypeRefs);
    for (uint32_t i = 1; i <= numTypeRefs; ++i) {
        auto row = tables_->typeRef(i);
        std::string ns   = heaps_->strings.get(row.ns);
        std::string name = heaps_->strings.get(row.name);
        typeRefNames_[i - 1] = makeClrName(ns, name);
    }

    return true;
}

std::string CLIReader::makeClrName(const std::string& ns, const std::string& name) const {
    if (ns.empty()) return name;
    return ns + "." + name;
}

// ─── buildClass ───────────────────────────────────────────────────────────────

BcClass CLIReader::buildClass(uint32_t typeDefIdx, CliReadResult& result) const {
    auto row = tables_->typeDef(typeDefIdx);
    BcClass cls;
    cls.fqName = typeDefName(typeDefIdx);
    // Simple name = after last '.' or '+'
    cls.name = cls.fqName;
    auto dot = cls.fqName.find_last_of(".+");
    if (dot != std::string::npos)
        cls.name = cls.fqName.substr(dot + 1);
    // Package name
    auto dot2 = cls.fqName.find_last_of('.');
    if (dot2 != std::string::npos)
        cls.packageName = cls.fqName.substr(0, dot2);

    // Access flags
    cls.access = typeDefAccess(row.flags);

    // is interface / abstract / sealed
    cls.isInterface = (row.flags & TypeAttributes::ClassSemanticsMask) == TypeAttributes::Interface;
    cls.isAbstract  = (row.flags & TypeAttributes::Abstract) != 0;

    // Superclass
    if (row.extends.valid()) {
        auto sup = resolveExtends(row.extends);
        if (sup) cls.superClass = *sup;
    }

    // Interfaces
    cls.interfaces = resolveInterfaces(typeDefIdx);

    // Generic parameters
    if (opts_.decodeGenericParams) {
        auto gps = genericParams(typeDefIdx, false);
        for (const auto& gp : gps)
            cls.typeParams.push_back(gp);
    }

    // Fields
    uint32_t numTypeDefs = tables_->rowCount(TableId::TypeDef);
    uint32_t fieldEnd;
    if (typeDefIdx < numTypeDefs)
        fieldEnd = tables_->typeDef(typeDefIdx + 1).fieldList;
    else
        fieldEnd = tables_->rowCount(TableId::Field) + 1;

    for (uint32_t fi = row.fieldList; fi < fieldEnd; ++fi) {
        try {
            auto f = buildField(fi);
            cls.fields.push_back(std::move(f));
        } catch (...) { ++result.parseErrorCount; }
    }
    result.fieldCount += static_cast<uint32_t>(cls.fields.size());

    // Methods
    uint32_t methodEnd;
    if (typeDefIdx < numTypeDefs)
        methodEnd = tables_->typeDef(typeDefIdx + 1).methodList;
    else
        methodEnd = tables_->rowCount(TableId::MethodDef) + 1;

    for (uint32_t mi = row.methodList; mi < methodEnd; ++mi) {
        try {
            auto m = buildMethod(mi, result);
            cls.methods.push_back(std::move(m));
        } catch (...) { ++result.parseErrorCount; }
    }
    result.methodDefCount += static_cast<uint32_t>(cls.methods.size());

    // Custom attributes
    if (opts_.decodeCustomAttrs) {
        MetadataToken parent{static_cast<uint8_t>(TableId::TypeDef), typeDefIdx};
        cls.annotations = customAttributes(parent);
    }

    return cls;
}

BcField CLIReader::buildField(uint32_t fieldIdx) const {
    auto row = tables_->field(fieldIdx);
    BcField f;
    f.name   = heaps_->strings.get(row.name);
    f.access = fieldAccess(row.flags);

    auto blob = heaps_->blobs.get(row.signature);
    if (!blob.empty()) {
        auto ct = sigDecoder_->decodeField(blob);
        if (ct) f.type = ct->base;
    }
    return f;
}

BcMethod CLIReader::buildMethod(uint32_t methodDefIdx, CliReadResult& result) const {
    auto row = tables_->methodDef(methodDefIdx);
    BcMethod m;
    m.name   = heaps_->strings.get(row.name);
    m.access = methodDefAccess(row.flags);
    m.isAbstract = (row.flags & MethodAttributes::Abstract) != 0;
    m.isNative   = (row.flags & MethodAttributes::PInvokeImpl) != 0;
    m.isConstructor = (m.name == ".ctor");
    m.isStaticInit  = (m.name == ".cctor");

    // Signature
    auto sigBlob = heaps_->blobs.get(row.signature);
    if (!sigBlob.empty()) {
        auto sig = sigDecoder_->decodeMethod(sigBlob);
        if (sig) {
            m.descriptor = sigDecoder_->toBcFuncType(*sig);
            // Generic method params
            if (sig->isGeneric && opts_.decodeGenericParams) {
                auto gps = genericParams(methodDefIdx, true);
                for (const auto& gp : gps)
                    m.typeParams.push_back(gp);
            }
        }
    }

    // Parameter names from Param table
    uint32_t numMethodDefs = tables_->rowCount(TableId::MethodDef);
    uint32_t paramEnd;
    if (methodDefIdx < numMethodDefs)
        paramEnd = tables_->methodDef(methodDefIdx + 1).paramList;
    else
        paramEnd = tables_->rowCount(TableId::Param) + 1;

    for (uint32_t pi = row.paramList; pi < paramEnd; ++pi) {
        auto prow = tables_->param(pi);
        if (prow.sequence == 0) continue;  // sequence 0 = return type param
        std::string pname = heaps_->strings.get(prow.name);
        if (!pname.empty() && prow.sequence - 1 < m.descriptor.params.size())
            m.paramNames.push_back(pname);
        else
            m.paramNames.push_back("p" + std::to_string(prow.sequence));
    }

    // CIL method body
    if (opts_.decodeCIL && row.rva != 0 && cilLifter_) {
        auto bodySpan = pe_.rvaToSpan(row.rva);
        if (!bodySpan.empty()) {
            CILMethodHeader hdr;
            m.cfg = cilLifter_->lift(bodySpan, hdr);
            m.maxStack = hdr.maxStack;

            // Local variable types from LocalVarSig
            if (hdr.localVarSigTok != 0) {
                MetadataToken tok = MetadataToken::fromRaw(hdr.localVarSigTok);
                if (tok.table == static_cast<uint8_t>(TableId::StandAloneSig)) {
                    auto sasRow = tables_->standAloneSig(tok.index);
                    auto lvBlob = heaps_->blobs.get(sasRow.signature);
                    auto localSig = sigDecoder_->decodeLocalVar(lvBlob);
                    if (localSig) {
                        uint32_t localIdx = 0;
                        for (const auto& lv : localSig->locals) {
                            BcLocalVar var;
                            var.index = localIdx++;
                            var.name  = "loc" + std::to_string(var.index);
                            var.type  = lv.base;
                            m.locals.push_back(var);
                        }
                    }
                }
            }
        }
    }

    return m;
}

// ─── Helpers ──────────────────────────────────────────────────────────────────

std::optional<BcType> CLIReader::resolveExtends(MetadataToken tok) const {
    if (!tok.valid()) return std::nullopt;
    std::string name;
    if (tok.table == static_cast<uint8_t>(TableId::TypeDef))
        name = typeDefName(tok.index);
    else if (tok.table == static_cast<uint8_t>(TableId::TypeRef))
        name = typeRefName(tok.index);
    else if (tok.table == static_cast<uint8_t>(TableId::TypeSpec))
        return typeSpecType(tok.index);
    else
        return std::nullopt;

    // Don't include implicit System.Object inheritance
    if (name == "System.Object") return std::nullopt;
    return CliSigDecoder::clrNameToType(name);
}

std::vector<BcType> CLIReader::resolveInterfaces(uint32_t typeDefIdx) const {
    std::vector<BcType> result;
    uint32_t n = tables_->rowCount(TableId::InterfaceImpl);
    for (uint32_t i = 1; i <= n; ++i) {
        auto row = tables_->interfaceImpl(i);
        if (row.clazz != typeDefIdx) continue;
        if (!row.interface_.valid()) continue;

        std::string name;
        if (row.interface_.table == static_cast<uint8_t>(TableId::TypeDef))
            name = typeDefName(row.interface_.index);
        else if (row.interface_.table == static_cast<uint8_t>(TableId::TypeRef))
            name = typeRefName(row.interface_.index);
        else
            continue;

        result.push_back(CliSigDecoder::clrNameToType(name));
    }
    return result;
}

std::vector<std::string> CLIReader::genericParams(uint32_t owner, bool isMethod) const {
    std::vector<std::string> result;
    uint32_t n = tables_->rowCount(TableId::GenericParam);
    uint8_t expectedTable = isMethod
        ? static_cast<uint8_t>(TableId::MethodDef)
        : static_cast<uint8_t>(TableId::TypeDef);

    std::vector<std::pair<uint16_t, std::string>> params;
    for (uint32_t i = 1; i <= n; ++i) {
        auto row = tables_->genericParam(i);
        if (row.owner.table == expectedTable && row.owner.index == owner) {
            std::string pname = heaps_->strings.get(row.name);
            if (pname.empty()) pname = "T" + std::to_string(row.number);
            params.emplace_back(row.number, pname);
        }
    }

    std::sort(params.begin(), params.end(),
              [](const auto& a, const auto& b){ return a.first < b.first; });
    for (const auto& [num, name] : params)
        result.push_back(name);

    return result;
}

std::vector<BcAnnotation> CLIReader::customAttributes(MetadataToken parent) const {
    std::vector<BcAnnotation> result;
    if (!opts_.decodeCustomAttrs) return result;

    uint32_t n = tables_->rowCount(TableId::CustomAttribute);
    for (uint32_t i = 1; i <= n; ++i) {
        auto row = tables_->customAttribute(i);
        // Check if this attribute is attached to our parent
        // The parent is encoded as a HasCustomAttribute coded token
        // We stored it as table/index in fields[0]/[1]
        if (row.parent.table != parent.table ||
            row.parent.index != parent.index) continue;

        BcAnnotation ann;
        // Resolve type name from CustomAttributeType
        if (row.type.valid()) {
            if (row.type.table == static_cast<uint8_t>(TableId::MethodDef)) {
                // Constructor MethodDef → find owning TypeDef
                ann.typeName = "<ctor>";
            } else if (row.type.table == static_cast<uint8_t>(TableId::MemberRef)) {
                auto mref = tables_->memberRef(row.type.index);
                if (mref.clazz.table == static_cast<uint8_t>(TableId::TypeRef))
                    ann.typeName = typeRefName(mref.clazz.index);
                else
                    ann.typeName = "<attr>";
            }
        }

        result.push_back(std::move(ann));
    }
    return result;
}

// ─── Access flag mapping ──────────────────────────────────────────────────────

BcAccess CLIReader::typeDefAccess(uint32_t flags) {
    uint32_t vis = flags & TypeAttributes::VisibilityMask;
    switch (vis) {
    case TypeAttributes::Public:
    case TypeAttributes::NestedPublic:     return BcAccess::Public;
    case TypeAttributes::NestedFamily:
    case TypeAttributes::NestedFamORAssem: return BcAccess::Protected;
    case TypeAttributes::NestedPrivate:    return BcAccess::Private;
    case TypeAttributes::NestedAssembly:
    case TypeAttributes::NestedFamANDAssem:return BcAccess::Internal;
    default:                               return BcAccess::Internal;
    }
}

BcAccess CLIReader::methodDefAccess(uint16_t flags) {
    uint16_t acc = flags & MethodAttributes::MemberAccessMask;
    switch (acc) {
    case MethodAttributes::Public:   return BcAccess::Public;
    case MethodAttributes::Family:
    case MethodAttributes::FamORAssem: return BcAccess::Protected;
    case MethodAttributes::Private:  return BcAccess::Private;
    case MethodAttributes::Assem:    return BcAccess::Internal;
    default:                         return BcAccess::Private;
    }
}

BcAccess CLIReader::fieldAccess(uint16_t flags) {
    uint16_t acc = flags & 0x0007;  // FieldAttributes MemberAccessMask
    switch (acc) {
    case 0x0006: return BcAccess::Public;
    case 0x0004: return BcAccess::Protected;
    case 0x0001: return BcAccess::Private;
    case 0x0003: return BcAccess::Internal;
    default:     return BcAccess::Private;
    }
}

bool CLIReader::isNested(uint32_t typeDefIdx) const {
    for (const auto& [n, e] : nestedMap_)
        if (n == typeDefIdx) return true;
    return false;
}

uint32_t CLIReader::enclosingClass(uint32_t typeDefIdx) const {
    for (const auto& [n, e] : nestedMap_)
        if (n == typeDefIdx) return e;
    return 0;
}

} // namespace cli_parser
} // namespace retdec
