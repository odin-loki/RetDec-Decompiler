/**
 * @file src/call_conv/variadic_detect.cpp
 * @brief Variadic function detection for SysV / Win64 / x86-32.
 *
 * ## Signals used
 *
 * ### Linux SysV AMD64
 *
 * The System V AMD64 ABI requires that `AL` (the low byte of RAX) holds the
 * number of XMM registers used for variadic arguments when calling a variadic
 * function.  At function *entry*, if AL is live-in, the function is variadic.
 *
 * Additionally, variadic functions almost always access the "register save
 * area" — 6 × 8-byte slots immediately above the red zone where the callee
 * spills its XMM arguments.  We detect this by looking for MemRef accesses
 * with negative high offsets from RBP (offsets in the range [-176, -48]).
 *
 * ### Windows x64
 *
 * va_list is defined as `char*` in MSVC.  The compiler generates:
 *
 *   LEA  va_ptr, [RSP + shadow_offset]  ; initialise va_list
 *   MOV  reg, [va_ptr]                  ; va_arg: load value
 *   ADD  va_ptr, 8                      ; advance va_list
 *
 * We detect this by searching for an ADD instruction operating on a pointer
 * value with a constant stride of 8 applied repeatedly (3+ times in a loop
 * → clear signal).
 *
 * ### x86-32 cdecl
 *
 * A cdecl function is variadic if it accesses stack memory beyond the last
 * named argument offset.  We detect this by finding the highest stack offset
 * used and comparing it with the number of named arguments × 4.
 */

#include "retdec/call_conv/call_conv.h"
#include "retdec/ssa/ssa.h"

#include <unordered_map>

namespace retdec {
namespace call_conv {

// ─── VariadicDetector::checkSysVAl ───────────────────────────────────────────

bool VariadicDetector::checkSysVAl(const ssa::SSAFunction& fn) const {
    // AL (low byte of RAX) live-in at entry → variadic.
    const ssa::BasicBlock* entry = fn.block(fn.entryId());
    if (!entry) return false;

    uint32_t alVar = fn.findVar("al");
    if (alVar != ssa::kInvalidVar && entry->liveIn.count(alVar)) {
        return true;
    }

    // Also check RAX itself — some analyses fold AL into RAX.
    uint32_t raxVar = fn.findVar("rax");
    if (raxVar != ssa::kInvalidVar && entry->liveIn.count(raxVar)) {
        // Extra confirmation: look for register-save area access.
        // Offsets [-176, -48] from RBP indicate XMM save area.
        for (const auto& blk : fn.blocks()) {
            if (!blk) continue;
            for (const ssa::IrInstr* instr : blk->instrs) {
                if (!instr) continue;
                if (instr->op == ssa::IrInstr::Op::Store ||
                    instr->op == ssa::IrInstr::Op::Load) {
                    for (const auto& use : instr->uses) {
                        const ssa::IrValue* val = fn.value(use.valueId);
                        if (!val) continue;
                        if (val->kind == ssa::ValueKind::MemRef &&
                            val->memIsStack &&
                            val->memOffset >= -176 &&
                            val->memOffset <= -48) {
                            return true;
                        }
                    }
                }
            }
        }
    }

    return false;
}

// ─── VariadicDetector::checkWin64VaList ──────────────────────────────────────

bool VariadicDetector::checkWin64VaList(const ssa::SSAFunction& fn) const {
    // Look for the va_list advancement pattern: repeated ADD by stride 8
    // applied to the same SSA value (pointer into shadow space).
    std::unordered_map<uint32_t, int> addCount;

    for (const auto& blk : fn.blocks()) {
        if (!blk) continue;
        for (const ssa::IrInstr* instr : blk->instrs) {
            if (!instr) continue;
            if (instr->op != ssa::IrInstr::Op::Add) continue;
            // Check if this Add has an immediate operand of 8.
            for (const auto& use : instr->uses) {
                const ssa::IrValue* val = fn.value(use.valueId);
                if (val && val->kind == ssa::ValueKind::Immediate
                        && val->imm == 8) {
                    // The other operand is the pointer being advanced.
                    for (const auto& u2 : instr->uses) {
                        if (u2.valueId != use.valueId) {
                            ++addCount[u2.valueId];
                        }
                    }
                }
            }
        }
    }

    // If any pointer has 3+ stride-8 advances, it's a va_list.
    for (const auto& [vid, cnt] : addCount) {
        if (cnt >= 3) return true;
    }
    return false;
}

// ─── VariadicDetector::checkX86CdeclExtendedStack ────────────────────────────

bool VariadicDetector::checkX86CdeclExtendedStack(
        const ssa::SSAFunction& fn, int numNamedArgs) const {
    // On x86-32 cdecl, args start at [EBP+8].  Named args occupy bytes
    // [EBP+8] through [EBP+8 + (numNamedArgs-1)*4].  Any stack access at
    // a higher offset indicates extra (variadic) arguments.
    const int32_t namedArgLimit = 8 + numNamedArgs * 4;

    for (const auto& blk : fn.blocks()) {
        if (!blk) continue;
        for (const ssa::IrInstr* instr : blk->instrs) {
            if (!instr) continue;
            if (instr->op != ssa::IrInstr::Op::Load &&
                instr->op != ssa::IrInstr::Op::Store) continue;
            for (const auto& use : instr->uses) {
                const ssa::IrValue* val = fn.value(use.valueId);
                if (!val) continue;
                if (val->kind == ssa::ValueKind::MemRef &&
                    val->memIsStack &&
                    val->memOffset >= namedArgLimit) {
                    return true;
                }
            }
        }
    }
    return false;
}

// ─── VariadicDetector::run ────────────────────────────────────────────────────

bool VariadicDetector::run(const ssa::SSAFunction& fn, CC cc) const {
    switch (cc) {
    case CC::SysVAmd64:
        return checkSysVAl(fn);

    case CC::Win64:
        return checkWin64VaList(fn);

    case CC::Cdecl:
        // For x86-32 cdecl, we use 0 named args as baseline; the
        // caller supplies argument count from RegArgAnalysis / stack analysis.
        return checkX86CdeclExtendedStack(fn, 0);

    default:
        return false;
    }
}

} // namespace call_conv
} // namespace retdec
