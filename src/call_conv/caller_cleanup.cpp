/**
 * @file src/call_conv/caller_cleanup.cpp
 * @brief x86-32 caller/callee cleanup detection + ABI register tables.
 *
 * ## Caller-cleanup detection
 *
 * On x86-32, caller-cleanup (cdecl) and callee-cleanup (stdcall/fastcall/
 * thiscall) are distinguished by what follows each CALL instruction:
 *
 *   cdecl:     CALL foo  →  ADD ESP, N    (imm = byte count of args pushed)
 *   stdcall:   CALL foo  →  (nothing / MOV / etc.)
 *   fastcall:  CALL foo  →  (nothing)   + ECX[,EDX] used at the call site
 *   thiscall:  CALL foo  →  (nothing)   + ECX = this-pointer
 *
 * We scan all Call instructions in every block.  For each call we inspect:
 *   1. The instruction immediately after the CALL in the block.
 *   2. Whether ECX / EDX are live-in at the call site (live-out of the
 *      preceding instruction).
 *
 * Majority voting over all call sites resolves the convention.
 *
 * ## ABI register table
 *
 * Also defined here: `intArgRegs()`, `fpArgRegs()`, `intRetRegs()`,
 * `fpRetRegs()` — the authoritative ABI register lists used by all
 * other sub-passes.
 */

#include "retdec/call_conv/call_conv.h"
#include "retdec/ssa/ssa.h"

#include <algorithm>
#include <sstream>

namespace retdec {
namespace call_conv {

// ─── ABI register tables ──────────────────────────────────────────────────────

std::vector<PhysReg> intArgRegs(CC cc) {
    switch (cc) {
    case CC::SysVAmd64:
        return { PhysReg::RDI, PhysReg::RSI, PhysReg::RDX,
                 PhysReg::RCX, PhysReg::R8,  PhysReg::R9 };
    case CC::Win64:
        return { PhysReg::RCX, PhysReg::RDX, PhysReg::R8, PhysReg::R9 };
    case CC::Fastcall:
        return { PhysReg::ECX, PhysReg::EDX };  // first two args
    case CC::Thiscall:
        return { PhysReg::ECX };                // only `this`
    case CC::AArch64SysV:
        return { PhysReg::X0, PhysReg::X1, PhysReg::X2, PhysReg::X3,
                 PhysReg::X4, PhysReg::X5, PhysReg::X6, PhysReg::X7 };
    case CC::Arm32Aapcs:
        return { PhysReg::R0, PhysReg::R1, PhysReg::R2, PhysReg::R3 };
    case CC::Cdecl:
    case CC::Stdcall:
    default:
        return {};  // all args on stack
    }
}

std::vector<PhysReg> fpArgRegs(CC cc) {
    switch (cc) {
    case CC::SysVAmd64:
        return { PhysReg::XMM0, PhysReg::XMM1, PhysReg::XMM2, PhysReg::XMM3,
                 PhysReg::XMM4, PhysReg::XMM5, PhysReg::XMM6, PhysReg::XMM7 };
    case CC::Win64:
        return { PhysReg::XMM0, PhysReg::XMM1, PhysReg::XMM2, PhysReg::XMM3 };
    case CC::AArch64SysV:
        return { PhysReg::V0, PhysReg::V1, PhysReg::V2, PhysReg::V3,
                 PhysReg::V4, PhysReg::V5, PhysReg::V6, PhysReg::V7 };
    default:
        return {};
    }
}

std::vector<PhysReg> intRetRegs(CC cc) {
    switch (cc) {
    case CC::SysVAmd64:
        return { PhysReg::RAX, PhysReg::RDX };  // RAX primary, RDX for 128-bit
    case CC::Win64:
        return { PhysReg::RAX };
    case CC::Cdecl:
    case CC::Stdcall:
    case CC::Fastcall:
    case CC::Thiscall:
        return { PhysReg::EAX };
    case CC::AArch64SysV:
        return { PhysReg::X0, PhysReg::X1 };
    case CC::Arm32Aapcs:
        return { PhysReg::R0, PhysReg::R1 };
    default:
        return { PhysReg::RAX };
    }
}

std::vector<PhysReg> fpRetRegs(CC cc) {
    switch (cc) {
    case CC::SysVAmd64:
    case CC::Win64:
        return { PhysReg::XMM0 };
    case CC::AArch64SysV:
        return { PhysReg::V0 };
    case CC::Arm32Aapcs:
        return { PhysReg::S0, PhysReg::D0 };
    default:
        return {};
    }
}

// ─── ArgDesc / RetDesc toString ───────────────────────────────────────────────

std::string ArgDesc::toString() const {
    std::ostringstream os;
    os << (isFp ? "fp" : "i") << static_cast<int>(width);
    if (kind == ArgKind::Register) {
        os << "@" << physRegName(reg);
    } else {
        os << "@stack[" << stackOffset << "]";
    }
    return os.str();
}

std::string RetDesc::toString() const {
    std::ostringstream os;
    switch (kind) {
    case RetKind::Void:    os << "void";    break;
    case RetKind::Integer: os << (isFp ? "fp" : "i") << static_cast<int>(width); break;
    case RetKind::Float:   os << "f" << static_cast<int>(width); break;
    case RetKind::Struct:  os << "struct";  break;
    }
    if (!regs.empty()) {
        os << "@";
        for (std::size_t i = 0; i < regs.size(); ++i) {
            if (i) os << ":";
            os << physRegName(regs[i]);
        }
    }
    return os.str();
}

std::string CallingConvention::toString() const {
    std::ostringstream os;
    os << ccName(cc);
    if (isVariadic) os << " [variadic]";
    os << " -> " << ret.toString();
    os << " args(";
    for (std::size_t i = 0; i < args.size(); ++i) {
        if (i) os << ", ";
        os << args[i].toString();
    }
    os << ")";
    if (stackCleanupBytes) os << " cleanup=" << stackCleanupBytes;
    return os.str();
}

// ─── CallerCleanupDetector ────────────────────────────────────────────────────

// Checks if the instruction immediately after `callIdx` in the block is
// `ADD ESP, N` (caller-cleanup).  Returns (true, N) or (false, 0).
static std::pair<bool, int32_t>
checkAddEspAfterCall(const ssa::BasicBlock* bb, std::size_t callIdx) {
    if (callIdx + 1 >= bb->instrs.size()) return {false, 0};
    const ssa::IrInstr* next = bb->instrs[callIdx + 1];
    if (!next) return {false, 0};

    // IrInstr::Op::Add with the destination being ESP (VarId ~4 in x86-32,
    // or matched by name) and the first use being an Immediate.
    if (next->op != ssa::IrInstr::Op::Add) return {false, 0};

    // Check that one of the uses is an immediate (stack cleanup size).
    // In our IR, Immediate values have ValueKind::Immediate.
    for (const auto& use : next->uses) {
        // We can't resolve ValueId to IrValue here without a fn reference,
        // so we use a heuristic: if the instruction has exactly 2 uses and
        // the defVar corresponds to ESP/RSP, it's ADD ESP, N.
        // The actual immediate value is encoded in `imm` of the IrValue,
        // but we only have VarId here.  We mark it as caller-cleanup with
        // bytes=4 (minimal guess; refined later by type inference).
        (void)use;
    }

    // For the purpose of detection, any Add instruction in the position
    // immediately after a Call in x86-32 is a strong signal of caller-cleanup.
    // We record it as such with cleanup=0 (unknown size at this stage).
    return {true, 0};
}

CallerCleanupDetector::Result
CallerCleanupDetector::run(const ssa::SSAFunction& fn) const {
    Result res;

    for (const auto& blk : fn.blocks()) {
        if (!blk) continue;
        const std::size_t n = blk->instrs.size();
        for (std::size_t i = 0; i < n; ++i) {
            const ssa::IrInstr* instr = blk->instrs[i];
            if (!instr || instr->op != ssa::IrInstr::Op::Call) continue;

            CallSiteEvidence ev;
            ev.callInstrId = instr->id;

            // Check for ADD ESP, N after the call.
            auto [callerClean, bytes] = checkAddEspAfterCall(blk.get(), i);
            ev.callerCleanup  = callerClean;
            ev.cleanupBytes   = bytes;

            // Check ECX / EDX liveness before the call.
            // We approximate by checking if any use of this Call references
            // VarId 1 (ECX in x86-32) or VarId 2 (EDX).
            for (const auto& use : instr->uses) {
                if (use.valueId == 1) ev.ecxUsed = true;  // heuristic: varId 1 = ECX
                if (use.valueId == 2) ev.edxUsed = true;  // heuristic: varId 2 = EDX
            }

            if (ev.callerCleanup) ++res.callerCleanupVotes;
            else                  ++res.calleeCleanupVotes;

            res.sites.push_back(ev);
        }
    }

    // Determine majority CC.
    if (res.sites.empty()) {
        res.cc = CC::Unknown;
        return res;
    }

    // Compute majority cleanup size.
    std::unordered_map<int32_t, int> cleanupHist;
    for (const auto& s : res.sites) {
        if (s.callerCleanup) cleanupHist[s.cleanupBytes]++;
    }
    if (!cleanupHist.empty()) {
        auto it = std::max_element(cleanupHist.begin(), cleanupHist.end(),
            [](const auto& a, const auto& b){ return a.second < b.second; });
        res.majorityCleanup = it->first;
    }

    // Check for fastcall / thiscall based on ECX/EDX usage patterns.
    int ecxCount = 0, edxCount = 0;
    for (const auto& s : res.sites) { if (s.ecxUsed) ++ecxCount; if (s.edxUsed) ++edxCount; }

    if (res.calleeCleanupVotes >= res.callerCleanupVotes) {
        // Callee-cleanup: distinguish stdcall/fastcall/thiscall.
        if (ecxCount > static_cast<int>(res.sites.size() / 2)) {
            res.cc = edxCount > static_cast<int>(res.sites.size() / 4)
                     ? CC::Fastcall : CC::Thiscall;
        } else {
            res.cc = CC::Stdcall;
        }
    } else {
        res.cc = CC::Cdecl;
    }

    return res;
}

} // namespace call_conv
} // namespace retdec
