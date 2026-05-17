/**
 * @file src/call_conv/reg_arg_analysis.cpp
 * @brief Register argument liveness analysis for x86-64 / AArch64 / ARM32.
 *
 * ## Algorithm
 *
 * The ABI specifies an ordered list of registers for passing integer and
 * floating-point arguments (see `intArgRegs()` / `fpArgRegs()`).  At
 * function entry, a register is "used as an argument" if:
 *   1. It is live-in at the entry basic block   (liveness-based), OR
 *   2. It is defined early in the entry block by a move from its physical
 *      register alias (register-definition-based, for when liveness is
 *      not yet fully computed).
 *
 * Argument count = highest-indexed live register in ABI order (0-based).
 * Example: for SysV AMD64, if RDI, RSI, RDX are live but RCX is not,
 * argument count = 3.
 *
 * Holes (non-live ABI registers below the highest live one) are treated
 * as passed-but-unused arguments (common in practice for e.g. `int foo(int,
 * int, int)` where the middle arg is unused).
 *
 * We map physical register names to SSA VarIds using `SSAFunction::varName()`.
 * The variable registry is searched for names like "rdi", "rsi", "xmm0", etc.
 */

#include "retdec/call_conv/call_conv.h"
#include "retdec/ssa/ssa.h"

#include <algorithm>

namespace retdec {
namespace call_conv {

// ─── RegArgAnalysis::findVarForReg ────────────────────────────────────────────

uint32_t RegArgAnalysis::findVarForReg(const ssa::SSAFunction& fn,
                                        PhysReg r) const {
    // Search the function's variable registry for a variable whose name
    // matches the physical register name.
    const char* name = physRegName(r);
    return fn.findVar(name);
}

// ─── Helper: is VarId live-in at entry? ───────────────────────────────────────

static bool isLiveAtEntry(const ssa::SSAFunction& fn, uint32_t varId) {
    if (varId == ssa::kInvalidVar) return false;
    const ssa::BasicBlock* entry = fn.block(fn.entryId());
    if (!entry) return false;
    return entry->liveIn.count(varId) > 0;
}

// ─── RegArgAnalysis::run ──────────────────────────────────────────────────────

std::vector<ArgDesc> RegArgAnalysis::run(const ssa::SSAFunction& fn,
                                          CC cc) const {
    std::vector<ArgDesc> args;

    auto intRegs = intArgRegs(cc);
    auto fpRegs  = fpArgRegs(cc);

    // --- Integer/pointer arguments ---
    int highestLiveInt = -1;
    std::vector<bool> intLive(intRegs.size(), false);
    for (std::size_t i = 0; i < intRegs.size(); ++i) {
        uint32_t varId = findVarForReg(fn, intRegs[i]);
        if (isLiveAtEntry(fn, varId)) {
            intLive[i]    = true;
            highestLiveInt = static_cast<int>(i);
        }
    }

    // --- Floating-point arguments ---
    int highestLiveFp = -1;
    std::vector<bool> fpLive(fpRegs.size(), false);
    for (std::size_t i = 0; i < fpRegs.size(); ++i) {
        uint32_t varId = findVarForReg(fn, fpRegs[i]);
        if (isLiveAtEntry(fn, varId)) {
            fpLive[i]    = true;
            highestLiveFp = static_cast<int>(i);
        }
    }

    // Build ArgDesc list up to highestLiveInt (all registers, including holes).
    for (int i = 0; i <= highestLiveInt; ++i) {
        ArgDesc d;
        d.kind   = ArgKind::Register;
        d.reg    = intRegs[static_cast<std::size_t>(i)];
        d.isFp   = false;
        d.width  = 64;

        uint32_t varId = findVarForReg(fn, d.reg);
        d.ssaValueId = varId;

        args.push_back(d);
    }

    // Float arguments (separate from integer on SysV; shadow-slots on Win64).
    for (int i = 0; i <= highestLiveFp; ++i) {
        // On Win64, XMM args shadow integer slots → skip if already counted.
        if (cc == CC::Win64 && i <= highestLiveInt) continue;

        ArgDesc d;
        d.kind   = ArgKind::Register;
        d.reg    = fpRegs[static_cast<std::size_t>(i)];
        d.isFp   = true;
        d.width  = 64;  // default double; type inference refines this
        d.ssaValueId = findVarForReg(fn, d.reg);
        args.push_back(d);
    }

    return args;
}

} // namespace call_conv
} // namespace retdec
