/**
 * @file src/call_conv/call_conv_pass.cpp
 * @brief CallConvPass orchestrator — assembles the full CallingConvention.
 *
 * ## Pass flow
 *
 * 1. Use the platform hint (`Config::platformCC`) as the starting convention.
 * 2. On x86-32: run `CallerCleanupDetector` to refine CC to cdecl/stdcall/
 *    fastcall/thiscall based on call-site evidence.
 * 3. Run `RegArgAnalysis` with the refined CC to count argument registers.
 * 4. Run `ReturnValueAnalysis` to determine return type.
 * 5. Run `VariadicDetector` to flag variadic functions.
 * 6. Assemble the final `CallingConvention` descriptor.
 *
 * Stack arguments:
 *   For x86-32 CCs (cdecl/stdcall/fastcall/thiscall), arguments beyond the
 *   register arguments are passed on the stack.  We emit stack ArgDescs with
 *   offsets starting at +8 from EBP (standard frame layout) in 4-byte steps,
 *   up to the cleanup byte count for caller-cleanup, or up to the callee RET N
 *   for callee-cleanup.
 *
 *   For x86-64 / AArch64, all remaining args beyond the register set are at
 *   increasing positive offsets from RSP at the call site; we emit stack descs
 *   only when evidence of such accesses is found in the function body (from
 *   MemRef accesses at positive-RBP offsets > 16 for x64).
 */

#include "retdec/call_conv/call_conv.h"
#include "retdec/ssa/ssa.h"

#include <set>

namespace retdec {
namespace call_conv {

// ─── Helper: detect stack arguments beyond register args ─────────────────────

static std::vector<ArgDesc>
detectStackArgs(const ssa::SSAFunction& fn, CC cc, int numRegArgs) {
    std::vector<ArgDesc> stackArgs;

    // Only applicable to x86-32 stack-based CCs.
    if (cc != CC::Cdecl && cc != CC::Stdcall &&
        cc != CC::Fastcall && cc != CC::Thiscall) {
        return stackArgs;
    }

    // Find all MemRef accesses with positive offsets from EBP beyond the
    // first two arguments (EBP+0 = saved EBP, EBP+4 = ret addr, EBP+8 = arg0).
    constexpr int32_t kFirstArgOffset = 8;
    constexpr int32_t kArgStride      = 4;
    int32_t regArgBytes = numRegArgs * kArgStride;
    int32_t stackArgStart = kFirstArgOffset + regArgBytes;

    std::set<int32_t> seenOffsets;
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
                    val->memOffset >= stackArgStart) {
                    seenOffsets.insert(val->memOffset);
                }
            }
        }
    }

    for (int32_t off : seenOffsets) {
        ArgDesc d;
        d.kind        = ArgKind::Stack;
        d.reg         = PhysReg::Invalid;
        d.stackOffset = off;
        d.width       = 32;
        d.isFp        = false;
        stackArgs.push_back(d);
    }
    return stackArgs;
}

// ─── CallConvPass::accumStats ────────────────────────────────────────────────

void CallConvPass::accumStats(CC cc, bool variadic) const {
    ++stats_.totalFunctions;
    if (variadic) ++stats_.variadicFunctions;
    switch (cc) {
    case CC::Cdecl:       ++stats_.cdeclFunctions;    break;
    case CC::Stdcall:     ++stats_.stdcallFunctions;  break;
    case CC::Fastcall:    ++stats_.fastcallFunctions; break;
    case CC::Thiscall:    ++stats_.thiscallFunctions; break;
    case CC::SysVAmd64:   ++stats_.sysvFunctions;     break;
    case CC::Win64:       ++stats_.win64Functions;    break;
    default:              ++stats_.unknownFunctions;  break;
    }
}

// ─── CallConvPass::run ───────────────────────────────────────────────────────

CallingConvention CallConvPass::run(const ssa::SSAFunction& fn,
                                     const Config& cfg) const {
    CallingConvention result;
    CC cc = cfg.platformCC;

    // ── Step 1: Refine CC for x86-32 ────────────────────────────────────────
    if (cfg.is32bit) {
        CallerCleanupDetector cleanDet;
        auto cleanRes = cleanDet.run(fn);
        if (cleanRes.cc != CC::Unknown) {
            // Respect config allowances.
            if (!cfg.allowThiscall && cleanRes.cc == CC::Thiscall)
                cleanRes.cc = CC::Stdcall;
            if (!cfg.allowFastcall && cleanRes.cc == CC::Fastcall)
                cleanRes.cc = CC::Stdcall;
            cc = cleanRes.cc;
        } else {
            cc = CC::Cdecl;  // default for x86-32
        }
        result.stackCleanupBytes = cleanRes.majorityCleanup;
    }

    result.cc = cc;

    // ── Step 2: Register arguments ───────────────────────────────────────────
    RegArgAnalysis regArg;
    result.args = regArg.run(fn, cc);

    // ── Step 3: Stack arguments (x86-32 only) ────────────────────────────────
    {
        auto stackArgs = detectStackArgs(fn, cc,
                                          static_cast<int>(result.args.size()));
        for (auto& s : stackArgs) result.args.push_back(std::move(s));
    }

    // ── Step 4: Return value ─────────────────────────────────────────────────
    ReturnValueAnalysis retAna;
    result.ret = retAna.run(fn, cc);

    // ── Step 5: Variadic detection ───────────────────────────────────────────
    VariadicDetector varDet;
    result.isVariadic = varDet.run(fn, cc);

    accumStats(cc, result.isVariadic);
    return result;
}

// ─── CallConvPass::runAll ────────────────────────────────────────────────────

std::unordered_map<std::string, CallingConvention>
CallConvPass::runAll(const std::vector<const ssa::SSAFunction*>& fns,
                      const Config& cfg) const {
    std::unordered_map<std::string, CallingConvention> results;
    for (const ssa::SSAFunction* fn : fns) {
        if (!fn) continue;
        results[fn->name()] = run(*fn, cfg);
    }
    return results;
}

} // namespace call_conv
} // namespace retdec
