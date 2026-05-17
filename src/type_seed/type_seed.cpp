/**
 * @file src/type_seed/type_seed.cpp
 * @brief Core dispatcher and constraint builder implementation.
 */

#include <memory>
#include "retdec/type_seed/type_seed.h"

#include <algorithm>
#include <cassert>

// Forward declarations for each seeder's factory function
namespace retdec { namespace type_seed {
    std::unique_ptr<ITypeSeeder> makeItaniumSeeder();
    std::unique_ptr<ITypeSeeder> makeMsvcSeeder();
    std::unique_ptr<ITypeSeeder> makeRustSeeder();
    std::unique_ptr<ITypeSeeder> makeSwiftSeeder();
}}

namespace retdec {
namespace type_seed {

// ─── mangledCCName ────────────────────────────────────────────────────────────

const char* mangledCCName(MangledCC cc) noexcept {
    switch (cc) {
    case MangledCC::Unknown:    return "unknown";
    case MangledCC::Cdecl:      return "__cdecl";
    case MangledCC::Stdcall:    return "__stdcall";
    case MangledCC::Fastcall:   return "__fastcall";
    case MangledCC::Thiscall:   return "__thiscall";
    case MangledCC::Vectorcall: return "__vectorcall";
    case MangledCC::Clrcall:    return "__clrcall";
    case MangledCC::Pascal:     return "__pascal";
    case MangledCC::Watcall:    return "__watcall";
    case MangledCC::Regparm:    return "__regparm";
    case MangledCC::SysVAmd64:  return "sysv_amd64";
    case MangledCC::Win64:      return "win64";
    case MangledCC::AArch64:    return "aarch64";
    }
    return "unknown";
}

// ─── ITypeInferenceMgr::addSignature ─────────────────────────────────────────

void ITypeInferenceMgr::addSignature(const SignatureInfo& sig, uint64_t vma) {
    auto constraints = TypeSeedDispatcher::toConstraints(sig, vma);
    for (auto& c : constraints) addGroundTruthConstraint(c);
}

// ─── TypeSeedDispatcher ───────────────────────────────────────────────────────

TypeSeedDispatcher::TypeSeedDispatcher() = default;

void TypeSeedDispatcher::registerSeeder(std::unique_ptr<ITypeSeeder> seeder) {
    seeders_.push_back(std::move(seeder));
}

SignatureInfo TypeSeedDispatcher::process(const std::string& symbol,
                                           uint64_t            vma,
                                           ITypeInferenceMgr&  mgr) const
{
    for (auto& seeder : seeders_) {
        if (!seeder->accepts(symbol)) continue;
        SignatureInfo sig = seeder->extract(symbol);
        if (!sig.valid()) continue;
        auto constraints = toConstraints(sig, vma);
        for (auto& c : constraints) mgr.addGroundTruthConstraint(c);
        return sig;
    }
    return {};
}

uint32_t TypeSeedDispatcher::processBatch(
    const std::vector<std::pair<std::string, uint64_t>>& symbols,
    ITypeInferenceMgr& mgr) const
{
    uint32_t count = 0;
    for (auto& [sym, vma] : symbols) {
        SignatureInfo sig = process(sym, vma, mgr);
        if (sig.valid()) ++count;
    }
    return count;
}

SignatureInfo TypeSeedDispatcher::tryExtract(const std::string& symbol) const {
    for (auto& seeder : seeders_) {
        if (!seeder->accepts(symbol)) continue;
        SignatureInfo sig = seeder->extract(symbol);
        if (sig.valid()) return sig;
    }
    return {};
}

// ─── toConstraints ───────────────────────────────────────────────────────────

/*static*/ std::vector<TypeConstraint> TypeSeedDispatcher::toConstraints(
    const SignatureInfo& sig, uint64_t vma)
{
    std::vector<TypeConstraint> out;

    auto make = [&](ConstraintKind k) -> TypeConstraint& {
        TypeConstraint c;
        c.kind         = k;
        c.symbolVma    = vma;
        c.confidence   = 1.0f;
        c.sourceSymbol = sig.mangledName;
        out.push_back(std::move(c));
        return out.back();
    };

    // Calling convention (always emit if known)
    if (sig.callingConvention != MangledCC::Unknown) {
        auto& c  = make(ConstraintKind::CallingConvention);
        c.cc     = sig.callingConvention;
        c.typeStr= mangledCCName(sig.callingConvention);
    }

    // Return type
    if (!sig.returnType.empty()) {
        auto& c  = make(ConstraintKind::ReturnType);
        c.typeStr= sig.returnType;
    }

    // This pointer
    if (sig.hasThis && !sig.thisType.empty()) {
        auto& c  = make(ConstraintKind::ThisType);
        c.typeStr= sig.thisType;
    }

    // Parameters
    for (uint32_t i=0; i<(uint32_t)sig.params.size(); ++i) {
        const auto& pi = sig.params[i];
        if (pi.type.empty()) continue;
        auto& c       = make(ConstraintKind::ParamType);
        c.paramIndex  = i;
        // Reconstruct full type string with qualifiers
        std::string full;
        if (pi.isConst)    full += "const ";
        if (pi.isVolatile) full += "volatile ";
        full += pi.type;
        if (pi.ref == RefCategory::LValueRef)  full += "&";
        if (pi.ref == RefCategory::RValueRef)  full += "&&";
        c.typeStr     = full;
    }

    // Template arguments
    for (uint32_t i=0; i<(uint32_t)sig.templateArgs.size(); ++i) {
        const auto& ta = sig.templateArgs[i];
        if (ta.empty()) continue;
        auto& c       = make(ConstraintKind::TemplateArgType);
        c.paramIndex  = i;
        c.typeStr     = ta;
    }

    return out;
}

// ─── makeDefaultDispatcher ────────────────────────────────────────────────────

TypeSeedDispatcher makeDefaultDispatcher() {
    TypeSeedDispatcher d;
    // Order matters: Rust _R and legacy Rust _ZN...h<hash>E must be tested
    // BEFORE Itanium _Z to avoid misclassification.
    d.registerSeeder(makeRustSeeder());
    d.registerSeeder(makeSwiftSeeder());
    // MSVC '?' before Itanium '_Z'
    d.registerSeeder(makeMsvcSeeder());
    d.registerSeeder(makeItaniumSeeder());
    return d;
}

} // namespace type_seed
} // namespace retdec
