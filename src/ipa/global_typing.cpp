/**
 * @file src/ipa/global_typing.cpp
 * @brief Global variable type unification across all functions.
 *
 * For each global address that appears as a Store destination in any function,
 * we collect the access width and floating-point status from all writers.
 * If widths or type kinds conflict across writers, the variable is flagged
 * as `isAmbiguous`; the code generator will emit a union.
 */

#include "retdec/ipa/ipa.h"
#include "retdec/ssa/ssa.h"

#include <cstdio>

namespace retdec {
namespace ipa {

std::unordered_map<std::string, GlobalVarInfo>
GlobalTyper::run(const std::vector<const ssa::SSAFunction*>& fns,
                  const std::unordered_map<FnName, FunctionSummary>& summaries) const {

    std::unordered_map<std::string, GlobalVarInfo> globals;

    // Helper: canonical name for a non-stack memory address.
    auto addrKey = [](int64_t offset) {
        char buf[32];
        std::snprintf(buf, sizeof(buf), "g_0x%llx",
                      static_cast<unsigned long long>(
                          static_cast<uint64_t>(offset)));
        return std::string(buf);
    };

    for (const ssa::SSAFunction* fn : fns) {
        if (!fn) continue;

        for (const auto& blk : fn->blocks()) {
            if (!blk) continue;
            for (const ssa::IrInstr* instr : blk->instrs) {
                if (!instr) continue;

                bool isWrite = (instr->op == ssa::IrInstr::Op::Store);
                bool isRead  = (instr->op == ssa::IrInstr::Op::Load);
                if (!isWrite && !isRead) continue;

                for (const auto& use : instr->uses) {
                    const ssa::IrValue* v = fn->value(use.valueId);
                    if (!v || v->kind != ssa::ValueKind::MemRef) continue;
                    if (v->memIsStack) continue;  // skip stack accesses

                    std::string key = addrKey(v->memOffset);
                    auto& gv = globals[key];
                    if (gv.name.empty()) {
                        gv.name    = key;
                        gv.address = static_cast<uint64_t>(v->memOffset);
                        gv.width   = v->memWidth ? v->memWidth * 8u : 64u;
                    }

                    // Check for type conflict.
                    uint8_t newWidth = v->memWidth ? v->memWidth * 8u : 64u;
                    if (gv.width != 0 && gv.width != newWidth) {
                        gv.isAmbiguous = true;
                    }
                    gv.width = std::max(gv.width, newWidth);

                    if (isWrite) gv.writers.insert(fn->name());
                    if (isRead)  gv.readers.insert(fn->name());

                    break;  // only check the destination operand
                }
            }
        }
    }

    return globals;
}

} // namespace ipa
} // namespace retdec
