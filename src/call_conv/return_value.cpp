/**
 * @file src/call_conv/return_value.cpp
 * @brief Return value analysis — which registers carry the return value.
 *
 * ## Algorithm
 *
 * For each basic block that ends in a RET instruction:
 *   1. Examine `BasicBlock::liveOut` (or the instruction's use list).
 *   2. Check which ABI return registers are live at that point.
 *
 * ABI return register conventions:
 *
 *   SysV AMD64:
 *     Integer:  RAX (primary), RDX (high 64 bits of 128-bit value / second half)
 *     Float:    XMM0
 *     Void:     neither RAX nor XMM0 live-out
 *
 *   Win64:
 *     Integer:  RAX
 *     Float:    XMM0
 *     Void:     neither live-out
 *
 *   AArch64 SysV:
 *     Integer:  X0 (primary), X1 (second half for 128-bit)
 *     Float:    V0
 *
 *   ARM32 AAPCS:
 *     Integer:  R0 (primary), R1 (high 32 bits for 64-bit)
 *     Float:    S0 / D0
 *
 * Majority vote over all RET blocks determines the return descriptor.
 */

#include "retdec/call_conv/call_conv.h"
#include "retdec/ssa/ssa.h"

namespace retdec {
namespace call_conv {

// ─── Helper: is VarId live-out at a RET block? ───────────────────────────────

static uint32_t findVarByName(const ssa::SSAFunction& fn, const char* name) {
    return fn.findVar(name);
}

static bool isVarLiveOut(const ssa::BasicBlock* bb, uint32_t varId) {
    if (!bb || varId == ssa::kInvalidVar) return false;
    return bb->liveOut.count(varId) > 0;
}

// ─── ReturnValueAnalysis::run ─────────────────────────────────────────────────

RetDesc ReturnValueAnalysis::run(const ssa::SSAFunction& fn, CC cc) const {
    // Collect liveness evidence from every RET block.
    struct Evidence {
        bool intPrimary  = false;  // RAX / X0 / R0
        bool intSecondary= false;  // RDX / X1 / R1
        bool fp          = false;  // XMM0 / V0 / S0
    };

    std::vector<Evidence> evidences;

    for (const auto& blk : fn.blocks()) {
        if (!blk || blk->instrs.empty()) continue;
        const ssa::IrInstr* last = blk->instrs.back();
        if (!last || last->op != ssa::IrInstr::Op::Ret) continue;

        Evidence ev;

        // Look up register variables by canonical name.
        switch (cc) {
        case CC::SysVAmd64: {
            uint32_t vRax = findVarByName(fn, "rax");
            uint32_t vRdx = findVarByName(fn, "rdx");
            uint32_t vXmm0= findVarByName(fn, "xmm0");
            ev.intPrimary   = isVarLiveOut(blk.get(), vRax);
            ev.intSecondary = isVarLiveOut(blk.get(), vRdx);
            ev.fp           = isVarLiveOut(blk.get(), vXmm0);
            break;
        }
        case CC::Win64: {
            uint32_t vRax  = findVarByName(fn, "rax");
            uint32_t vXmm0 = findVarByName(fn, "xmm0");
            ev.intPrimary = isVarLiveOut(blk.get(), vRax);
            ev.fp         = isVarLiveOut(blk.get(), vXmm0);
            break;
        }
        case CC::Cdecl:
        case CC::Stdcall:
        case CC::Fastcall:
        case CC::Thiscall: {
            uint32_t vEax = findVarByName(fn, "eax");
            ev.intPrimary = isVarLiveOut(blk.get(), vEax);
            break;
        }
        case CC::AArch64SysV: {
            uint32_t vX0 = findVarByName(fn, "x0");
            uint32_t vX1 = findVarByName(fn, "x1");
            uint32_t vV0 = findVarByName(fn, "v0");
            ev.intPrimary   = isVarLiveOut(blk.get(), vX0);
            ev.intSecondary = isVarLiveOut(blk.get(), vX1);
            ev.fp           = isVarLiveOut(blk.get(), vV0);
            break;
        }
        case CC::Arm32Aapcs: {
            uint32_t vR0 = findVarByName(fn, "r0");
            uint32_t vR1 = findVarByName(fn, "r1");
            uint32_t vS0 = findVarByName(fn, "s0");
            ev.intPrimary   = isVarLiveOut(blk.get(), vR0);
            ev.intSecondary = isVarLiveOut(blk.get(), vR1);
            ev.fp           = isVarLiveOut(blk.get(), vS0);
            break;
        }
        default:
            break;
        }

        evidences.push_back(ev);
    }

    if (evidences.empty()) {
        // No RET found — treat as void (might be a noreturn function).
        return RetDesc{RetKind::Void, {}, 0, false};
    }

    // Majority vote.
    int intPrimVotes = 0, intSecVotes = 0, fpVotes = 0;
    for (const auto& ev : evidences) {
        if (ev.intPrimary)   ++intPrimVotes;
        if (ev.intSecondary) ++intSecVotes;
        if (ev.fp)           ++fpVotes;
    }
    const int total = static_cast<int>(evidences.size());
    bool useInt = intPrimVotes >  total / 2;
    bool useSec = intSecVotes  >  total / 2;
    bool useFp  = fpVotes      >  total / 2;

    RetDesc ret;
    if (!useInt && !useFp) {
        ret.kind = RetKind::Void;
        ret.width = 0;
    } else if (useFp && !useInt) {
        ret.kind  = RetKind::Float;
        ret.isFp  = true;
        ret.width = 64;
        ret.regs  = fpRetRegs(cc);
    } else if (useInt && useSec) {
        ret.kind  = RetKind::Struct;
        ret.width = 128;
        ret.regs  = intRetRegs(cc);
    } else {
        ret.kind  = RetKind::Integer;
        ret.width = 64;
        ret.regs  = { intRetRegs(cc).front() };
    }

    return ret;
}

} // namespace call_conv
} // namespace retdec
