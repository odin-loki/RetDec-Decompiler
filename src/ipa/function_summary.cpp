/**
 * @file src/ipa/function_summary.cpp
 * @brief Per-function summary computation from intra-procedural analysis results.
 *
 * The summary captures everything a caller needs to know about a callee:
 *   - Parameter types and pointer properties (from calling convention + type info).
 *   - Return type.
 *   - Which params escape (their address is taken inside the function).
 *   - Which params are written through (output parameters).
 *   - Global variables read and written.
 *   - Derived flags: isPure, isNoReturn.
 *   - Size information: instruction count, call count.
 */

#include "retdec/ipa/ipa.h"
#include "retdec/ssa/ssa.h"
#include "retdec/call_conv/call_conv.h"
#include <algorithm>

namespace retdec {
namespace ipa {

// ─── SummaryComputer::compute ─────────────────────────────────────────────────

FunctionSummary SummaryComputer::compute(
        const ssa::SSAFunction& fn,
        const call_conv::CallingConvention& cc) const {

    FunctionSummary s;
    s.name = fn.name();

    // ── Parameter info ───────────────────────────────────────────────────────
    for (const call_conv::ArgDesc& arg : cc.args) {
        FunctionSummary::ParamInfo p;
        p.width     = arg.width;
        p.isFp      = arg.isFp;
        p.isPointer = false;  // refined later by type inference if available

        // Check if this argument's SSA value is used as a Store destination
        // inside the function → indicates pointer param + write.
        if (arg.ssaValueId != ssa::kInvalidValue) {
            for (const auto& blk : fn.blocks()) {
                if (!blk) continue;
                for (const ssa::IrInstr* instr : blk->instrs) {
                    if (!instr) continue;
                    if (instr->op == ssa::IrInstr::Op::Store && !instr->uses.empty()) {
                        if (instr->uses[0].valueId == arg.ssaValueId) {
                            p.isPointer  = true;
                            p.isModified = true;
                        }
                    }
                    // Escape: if the arg value is passed as an argument to
                    // another function, it may escape.
                    if (instr->op == ssa::IrInstr::Op::Call) {
                        for (const auto& use : instr->uses) {
                            if (use.valueId == arg.ssaValueId) {
                                p.escapes = true;
                            }
                        }
                    }
                }
            }
        }
        s.params.push_back(p);
    }

    // ── Return type ──────────────────────────────────────────────────────────
    s.isVoid   = (cc.ret.kind == call_conv::RetKind::Void);
    s.retIsFp  = cc.ret.isFp;
    s.retWidth = cc.ret.width;
    s.retIsPtr = false;

    // ── Global reads / writes ────────────────────────────────────────────────
    for (const auto& blk : fn.blocks()) {
        if (!blk) continue;
        for (const ssa::IrInstr* instr : blk->instrs) {
            if (!instr) continue;
            if (instr->op == ssa::IrInstr::Op::Store) {
                for (const auto& use : instr->uses) {
                    const ssa::IrValue* v = fn.value(use.valueId);
                    if (v && v->kind == ssa::ValueKind::MemRef && !v->memIsStack) {
                        // Non-stack store = global write (address as hex string).
                        char buf[32];
                        std::snprintf(buf, sizeof(buf), "0x%llx",
                                      static_cast<unsigned long long>(v->memOffset));
                        s.globalWrites.insert(buf);
                    }
                }
            } else if (instr->op == ssa::IrInstr::Op::Load) {
                for (const auto& use : instr->uses) {
                    const ssa::IrValue* v = fn.value(use.valueId);
                    if (v && v->kind == ssa::ValueKind::MemRef && !v->memIsStack) {
                        char buf[32];
                        std::snprintf(buf, sizeof(buf), "0x%llx",
                                      static_cast<unsigned long long>(v->memOffset));
                        s.globalReads.insert(buf);
                    }
                }
            }
        }
    }

    // ── Derived flags ────────────────────────────────────────────────────────
    s.isPure = s.globalWrites.empty() &&
               std::none_of(s.params.begin(), s.params.end(),
                            [](const FunctionSummary::ParamInfo& p){
                                return p.escapes || p.isModified; });

    // noreturn: no reachable RET instruction.
    bool hasRet = false;
    for (const auto& blk : fn.blocks()) {
        if (!blk) continue;
        for (const ssa::IrInstr* instr : blk->instrs) {
            if (instr && instr->op == ssa::IrInstr::Op::Ret) {
                hasRet = true; break;
            }
        }
        if (hasRet) break;
    }
    s.isNoReturn = !hasRet;

    // ── Size ─────────────────────────────────────────────────────────────────
    for (const auto& blk : fn.blocks()) {
        if (!blk) continue;
        s.instrCount += blk->instrs.size();
    }

    return s;
}

// ─── GlobalVarInfo::toString ─────────────────────────────────────────────────

std::string GlobalVarInfo::toString() const {
    std::string s = name;
    s += " @0x" + std::to_string(address);
    s += " i" + std::to_string(width);
    if (isFp)        s += "(fp)";
    if (isPointer)   s += "*";
    if (isAmbiguous) s += " [ambiguous]";
    return s;
}

} // namespace ipa
} // namespace retdec
