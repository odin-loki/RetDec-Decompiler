/**
 * @file src/cli_parser/cli_sig.cpp
 * @brief .NET CLI type signature decoder.
 */

#include <memory>
#include "retdec/cli_parser/cli_sig.h"

#include <cassert>
#include <cstring>
#include <stdexcept>
#include <unordered_map>

namespace retdec {
namespace cli_parser {

// ─── CLR name → BcType mapping ────────────────────────────────────────────────

static const std::unordered_map<std::string, BcType> kClrToBC = {
    {"System.Void",    types::Void()},
    {"System.Boolean", types::Bool()},
    {"System.Char",    types::Char()},
    {"System.SByte",   types::Byte()},
    {"System.Byte",    types::UByte()},
    {"System.Int16",   types::Short()},
    {"System.UInt16",  types::UShort()},
    {"System.Int32",   types::Int()},
    {"System.UInt32",  types::UInt()},
    {"System.Int64",   types::Long()},
    {"System.UInt64",  types::ULong()},
    {"System.Single",  types::Float()},
    {"System.Double",  types::Double()},
    {"System.String",  types::ClrString()},
    {"System.Object",  types::ClrObject()},
    {"System.IntPtr",  types::Long()},
    {"System.UIntPtr", types::ULong()},
};

BcType CliSigDecoder::clrNameToType(const std::string& fqName) {
    auto it = kClrToBC.find(fqName);
    if (it != kClrToBC.end()) return it->second;
    return types::Class(fqName);
}

// ─── ElementType → BcPrimKind ─────────────────────────────────────────────────

std::optional<BcPrimKind> CliSigDecoder::elemToPrim(ElementType e) {
    switch (e) {
        case ElementType::Void:    return BcPrimKind::Void;
        case ElementType::Boolean: return BcPrimKind::Bool;
        case ElementType::Char:    return BcPrimKind::Char;
        case ElementType::I1:      return BcPrimKind::Byte;
        case ElementType::U1:      return BcPrimKind::UByte;
        case ElementType::I2:      return BcPrimKind::Short;
        case ElementType::U2:      return BcPrimKind::UShort;
        case ElementType::I4:      return BcPrimKind::Int;
        case ElementType::U4:      return BcPrimKind::UInt;
        case ElementType::I8:      return BcPrimKind::Long;
        case ElementType::U8:      return BcPrimKind::ULong;
        case ElementType::R4:      return BcPrimKind::Float;
        case ElementType::R8:      return BcPrimKind::Double;
        case ElementType::I:       return BcPrimKind::Long;   // IntPtr
        case ElementType::U:       return BcPrimKind::ULong;  // UIntPtr
        default:                   return std::nullopt;
    }
}

// ─── CliSigDecoder ────────────────────────────────────────────────────────────

CliSigDecoder::CliSigDecoder(const ITypeNameResolver* resolver)
    : resolver_(resolver) {}

std::string CliSigDecoder::tokenName(MetadataToken tok) const {
    if (!resolver_) return "?";
    if (tok.table == static_cast<uint8_t>(TableId::TypeDef))
        return resolver_->typeDefName(tok.index);
    if (tok.table == static_cast<uint8_t>(TableId::TypeRef))
        return resolver_->typeRefName(tok.index);
    if (tok.table == static_cast<uint8_t>(TableId::TypeSpec))
        return resolver_->typeSpecType(tok.index).toString();
    return "?";
}

MetadataToken CliSigDecoder::readTypeDefOrRef(
        std::span<const uint8_t> blob, size_t& pos) const {
    auto val = decodeCompressedUInt(blob, pos);
    if (!val) return {};
    uint32_t coded = *val;
    uint32_t tag = coded & 0x3;
    uint32_t idx = coded >> 2;
    static const uint8_t kTabs[] = {
        static_cast<uint8_t>(TableId::TypeDef),
        static_cast<uint8_t>(TableId::TypeRef),
        static_cast<uint8_t>(TableId::TypeSpec),
    };
    if (tag >= 3) return {};
    return {kTabs[tag], idx};
}

void CliSigDecoder::decodeCustomMods(
        std::span<const uint8_t> blob, size_t& pos,
        std::vector<MetadataToken>& reqs,
        std::vector<MetadataToken>& opts) const {
    while (pos < blob.size()) {
        uint8_t et = blob[pos];
        if (et != static_cast<uint8_t>(ElementType::CModReqd) &&
            et != static_cast<uint8_t>(ElementType::CModOpt))
            break;
        ++pos;
        MetadataToken tok = readTypeDefOrRef(blob, pos);
        if (et == static_cast<uint8_t>(ElementType::CModReqd))
            reqs.push_back(tok);
        else
            opts.push_back(tok);
    }
}

void CliSigDecoder::decodeArrayShape(
        std::span<const uint8_t> blob, size_t& pos,
        uint32_t& rank,
        std::vector<int32_t>& sizes,
        std::vector<int32_t>& loBounds) const {
    auto rankOpt = decodeCompressedUInt(blob, pos);
    rank = rankOpt.value_or(0);
    auto numSizes = decodeCompressedUInt(blob, pos).value_or(0);
    for (uint32_t i = 0; i < numSizes; ++i) {
        auto sz = decodeCompressedUInt(blob, pos).value_or(0);
        sizes.push_back(static_cast<int32_t>(sz));
    }
    auto numLoBounds = decodeCompressedUInt(blob, pos).value_or(0);
    for (uint32_t i = 0; i < numLoBounds; ++i) {
        auto lb = decodeCompressedInt(blob, pos).value_or(0);
        loBounds.push_back(lb);
    }
}

CliType CliSigDecoder::decodeType(
        std::span<const uint8_t> blob, size_t& pos) const {
    CliType ct;
    ct.base = types::Object();

    // Consume custom modifiers first
    decodeCustomMods(blob, pos, ct.modreqs, ct.modopts);

    if (pos >= blob.size()) return ct;

    // Check for Pinned / ByRef before the actual type
    while (pos < blob.size()) {
        uint8_t tag = blob[pos];
        if (tag == static_cast<uint8_t>(ElementType::Pinned)) {
            ct.pinned = true; ++pos;
        } else if (tag == static_cast<uint8_t>(ElementType::ByRef)) {
            ct.byRef = true; ++pos;
        } else {
            break;
        }
    }

    if (pos >= blob.size()) return ct;

    auto et = static_cast<ElementType>(blob[pos++]);

    // Primitive types
    auto primKind = elemToPrim(et);
    if (primKind.has_value()) {
        ct.base = BcType{BcPrimType{*primKind}};
        return ct;
    }

    switch (et) {
    case ElementType::String:
        ct.base = types::ClrString();
        return ct;

    case ElementType::Object:
        ct.base = types::ClrObject();
        return ct;

    case ElementType::TypedByRef:
        ct.typedByRef = true;
        ct.base = types::Class("System.TypedReference");
        return ct;

    case ElementType::ValueType:
    case ElementType::Class: {
        MetadataToken tok = readTypeDefOrRef(blob, pos);
        std::string name = tokenName(tok);
        ct.base = (et == ElementType::ValueType)
                    ? clrNameToType(name) : types::Class(name);
        return ct;
    }

    case ElementType::SzArray: {
        CliType elem = decodeType(blob, pos);
        ct.base = types::Array(elem.base, 1);
        return ct;
    }

    case ElementType::Array: {
        CliType elem = decodeType(blob, pos);
        uint32_t rank = 1;
        std::vector<int32_t> sizes, loBounds;
        decodeArrayShape(blob, pos, rank, sizes, loBounds);
        ct.base = types::Array(elem.base, static_cast<int>(rank));
        return ct;
    }

    case ElementType::GenericInst: {
        // CLASS or VALUETYPE + TypeDefOrRef + GenArgCount + Type*
        bool isValue = (pos < blob.size() &&
                        blob[pos] == static_cast<uint8_t>(ElementType::ValueType));
        ++pos;  // skip CLASS/VALUETYPE
        MetadataToken tok = readTypeDefOrRef(blob, pos);
        std::string baseName = tokenName(tok);
        auto countOpt = decodeCompressedUInt(blob, pos);
        uint32_t count = countOpt.value_or(0);
        std::vector<BcType> args;
        for (uint32_t i = 0; i < count; ++i) {
            CliType argType = decodeType(blob, pos);
            args.push_back(argType.base);
        }
        ct.base = types::Generic(types::Class(baseName), std::move(args));
        (void)isValue;
        return ct;
    }

    case ElementType::Ptr: {
        CliType inner = decodeType(blob, pos);
        ct.isPtr = true;
        ct.base = inner.base;
        return ct;
    }

    case ElementType::FnPtr: {
        // Decode inner MethodSig
        // Skip for now: just use Object
        ct.base = types::ClrObject();
        return ct;
    }

    case ElementType::Var: {
        auto idx = decodeCompressedUInt(blob, pos);
        ct.varIdx = static_cast<int>(idx.value_or(0));
        ct.isMVar = false;
        BcRefType ref;
        ref.kind = BcRefKind::TypeVariable;
        ref.className = "`" + std::to_string(ct.varIdx);
        ct.base = BcType{ref};
        return ct;
    }

    case ElementType::MVar: {
        auto idx = decodeCompressedUInt(blob, pos);
        ct.varIdx = static_cast<int>(idx.value_or(0));
        ct.isMVar = true;
        BcRefType ref;
        ref.kind = BcRefKind::TypeVariable;
        ref.className = "``" + std::to_string(ct.varIdx);
        ct.base = BcType{ref};
        return ct;
    }

    case ElementType::Sentinel:
    case ElementType::End:
    default:
        return ct;
    }
}

BcType CliSigDecoder::toBcType(const CliType& ct) const { return ct.base; }

std::optional<CliType> CliSigDecoder::decodeField(
        std::span<const uint8_t> blob) const {
    if (blob.empty() || blob[0] != 0x06) return std::nullopt;
    size_t pos = 1;
    return decodeType(blob, pos);
}

std::optional<CliMethodSig> CliSigDecoder::decodeMethod(
        std::span<const uint8_t> blob) const {
    if (blob.empty()) return std::nullopt;
    CliMethodSig sig;

    size_t pos = 0;
    uint8_t conv = blob[pos++];
    sig.hasThis      = (conv & static_cast<uint8_t>(CallingConvention::HasThis)) != 0;
    sig.explicitThis = (conv & static_cast<uint8_t>(CallingConvention::ExplicitThis)) != 0;
    sig.isGeneric    = (conv & static_cast<uint8_t>(CallingConvention::Generic)) != 0;
    sig.callingConv  = static_cast<CallingConvention>(conv & 0x0F);

    if (sig.isGeneric) {
        auto gc = decodeCompressedUInt(blob, pos);
        sig.genParamCount = gc.value_or(0);
    }

    auto paramCount = decodeCompressedUInt(blob, pos);
    uint32_t numParams = paramCount.value_or(0);

    // Return type
    sig.retType = decodeType(blob, pos);

    // Parameters
    for (uint32_t i = 0; i < numParams && pos < blob.size(); ++i) {
        if (blob[pos] == static_cast<uint8_t>(ElementType::Sentinel)) {
            sig.hasVarArg   = true;
            sig.varArgStart = i;
            ++pos;
        }
        sig.params.push_back(decodeType(blob, pos));
    }

    return sig;
}

BcFuncType CliSigDecoder::toBcFuncType(const CliMethodSig& sig) const {
    BcFuncType ft;
    for (const auto& p : sig.params)
        ft.params.push_back(std::make_shared<BcType>(p.base));
    if (sig.retType.base.isVoid())
        ft.returnType = nullptr;
    else
        ft.returnType = std::make_shared<BcType>(sig.retType.base);
    return ft;
}

std::optional<CliLocalVarSig> CliSigDecoder::decodeLocalVar(
        std::span<const uint8_t> blob) const {
    if (blob.empty() || blob[0] != 0x07) return std::nullopt;
    size_t pos = 1;
    CliLocalVarSig sig;
    auto count = decodeCompressedUInt(blob, pos);
    uint32_t n = count.value_or(0);
    for (uint32_t i = 0; i < n && pos < blob.size(); ++i)
        sig.locals.push_back(decodeType(blob, pos));
    return sig;
}

std::optional<CliType> CliSigDecoder::decodeTypeSpec(
        std::span<const uint8_t> blob) const {
    if (blob.empty()) return std::nullopt;
    size_t pos = 0;
    return decodeType(blob, pos);
}

std::vector<CliType> CliSigDecoder::decodeMethodSpec(
        std::span<const uint8_t> blob) const {
    std::vector<CliType> args;
    if (blob.size() < 2 || blob[0] != 0x0A) return args;
    size_t pos = 1;
    auto cnt = decodeCompressedUInt(blob, pos);
    uint32_t n = cnt.value_or(0);
    for (uint32_t i = 0; i < n && pos < blob.size(); ++i)
        args.push_back(decodeType(blob, pos));
    return args;
}

} // namespace cli_parser
} // namespace retdec
