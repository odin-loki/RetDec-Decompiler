/**
 * @file src/bc_module/bc_module.cpp
 * @brief BcModule, BcClass, BcMethod helpers.
 */

#include "retdec/bc_module/bc_module.h"

#include <algorithm>
#include <stdexcept>

namespace retdec {
namespace bc_module {

// ─── SourceLang ───────────────────────────────────────────────────────────────

std::string sourceLangName(SourceLang lang) noexcept {
    switch (lang) {
    case SourceLang::Java:        return "Java";
    case SourceLang::CSharp:      return "CSharp";
    case SourceLang::Python:      return "Python";
    case SourceLang::WebAssembly: return "WebAssembly";
    case SourceLang::Lua:         return "Lua";
    case SourceLang::Kotlin:      return "Kotlin";
    case SourceLang::Scala:       return "Scala";
    case SourceLang::Groovy:      return "Groovy";
    case SourceLang::Clojure:     return "Clojure";
    case SourceLang::FSharp:      return "FSharp";
    case SourceLang::VisualBasic: return "VisualBasic";
    default:                      return "Unknown";
    }
}

// ─── BcClass ──────────────────────────────────────────────────────────────────

BcMethod* BcClass::findMethod(const std::string& name, const std::string& desc) {
    for (auto& m : methods) {
        if (m.name != name) continue;
        if (!desc.empty() && m.descriptor.jvmDescriptor() != desc) continue;
        return &m;
    }
    return nullptr;
}

const BcMethod* BcClass::findMethod(const std::string& name, const std::string& desc) const {
    for (const auto& m : methods) {
        if (m.name != name) continue;
        if (!desc.empty() && m.descriptor.jvmDescriptor() != desc) continue;
        return &m;
    }
    return nullptr;
}

BcField* BcClass::findField(const std::string& name) {
    for (auto& f : fields) if (f.name == name) return &f;
    return nullptr;
}

const BcField* BcClass::findField(const std::string& name) const {
    for (const auto& f : fields) if (f.name == name) return &f;
    return nullptr;
}

// ─── BcModule ─────────────────────────────────────────────────────────────────

BcModule::BcModule(std::string name, SourceLang lang)
    : name_(std::move(name)), lang_(lang) {}

BcClass& BcModule::addClass(BcClass cls) {
    classes_.push_back(std::move(cls));
    return classes_.back();
}

BcClass* BcModule::findClass(const std::string& fqName) {
    for (auto& c : classes_) if (c.fqName == fqName) return &c;
    return nullptr;
}

const BcClass* BcModule::findClass(const std::string& fqName) const {
    for (const auto& c : classes_) if (c.fqName == fqName) return &c;
    return nullptr;
}

uint32_t BcModule::internString(const std::string& s) {
    auto it = stringIndex_.find(s);
    if (it != stringIndex_.end()) return it->second;
    uint32_t idx = static_cast<uint32_t>(stringPool_.size());
    stringPool_.push_back(s);
    stringIndex_[s] = idx;
    return idx;
}

const std::string& BcModule::string(uint32_t idx) const {
    return stringPool_.at(idx);
}

void BcModule::addExternalRef(const std::string& fqName, const std::string& importForm) {
    externalRefs_[fqName] = importForm;
}

bool BcModule::verify(std::string& error) const {
    for (const auto& cls : classes_) {
        for (const auto& method : cls.methods) {
            std::string cfgError;
            if (!method.cfg.verify(cfgError)) {
                error = cls.fqName + "::" + method.name + " — " + cfgError;
                return false;
            }
        }
    }
    return true;
}

} // namespace bc_module
} // namespace retdec
