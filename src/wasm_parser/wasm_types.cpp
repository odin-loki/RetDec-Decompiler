/**
 * @file src/wasm_parser/wasm_types.cpp
 */

#include "retdec/wasm_parser/wasm_types.h"

#include <algorithm>

namespace retdec {
namespace wasm_parser {

std::string valTypeStr(ValType vt) {
    switch (vt) {
    case ValType::I32:     return "i32";
    case ValType::I64:     return "i64";
    case ValType::F32:     return "f32";
    case ValType::F64:     return "f64";
    case ValType::V128:    return "v128";
    case ValType::FuncRef: return "funcref";
    case ValType::ExternRef: return "externref";
    }
    return "i32";
}

// ─── WasmModule helpers ───────────────────────────────────────────────────────

uint32_t WasmModule::importedFuncCount() const {
    uint32_t count = 0;
    for (const auto& imp : imports)
        if (imp.kind == ExternKind::Func) ++count;
    return count;
}

uint32_t WasmModule::totalFuncCount() const {
    return importedFuncCount() +
           static_cast<uint32_t>(funcTypeIndices.size());
}

uint32_t WasmModule::funcTypeIndex(uint32_t funcIdx) const {
    uint32_t imported = importedFuncCount();
    if (funcIdx < imported) {
        // Imported function
        uint32_t n = 0;
        for (const auto& imp : imports) {
            if (imp.kind == ExternKind::Func) {
                if (n == funcIdx) return imp.index;
                ++n;
            }
        }
        return 0;
    }
    uint32_t localIdx = funcIdx - imported;
    if (localIdx < funcTypeIndices.size())
        return funcTypeIndices[localIdx];
    return 0;
}

std::string WasmModule::funcName(uint32_t funcIdx) const {
    // 1. From name section
    if (names.has_value()) {
        for (const auto& nm : names->funcNames) {
            if (nm.index == funcIdx) return nm.name;
        }
    }
    // 2. From exports
    auto expName = funcExportName(funcIdx);
    if (expName.has_value()) return *expName;
    // 3. Synthetic
    return "func" + std::to_string(funcIdx);
}

std::string WasmModule::globalName(uint32_t idx) const {
    if (names.has_value()) {
        for (const auto& nm : names->globalNames) {
            if (nm.index == idx) return nm.name;
        }
    }
    for (const auto& exp : exports) {
        if (exp.kind == ExternKind::Global && exp.index == idx)
            return exp.name;
    }
    return "global" + std::to_string(idx);
}

std::optional<std::string> WasmModule::funcExportName(uint32_t funcIdx) const {
    for (const auto& exp : exports) {
        if (exp.kind == ExternKind::Func && exp.index == funcIdx)
            return exp.name;
    }
    return std::nullopt;
}

} // namespace wasm_parser
} // namespace retdec
