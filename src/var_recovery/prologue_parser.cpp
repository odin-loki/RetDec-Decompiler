/**
 * @file src/var_recovery/prologue_parser.cpp
 * @brief Prologue pattern recognition for x86-64 SysV, x86-64 Win64,
 *        x86-32, AArch64, and ARM32.
 *
 * ## Pattern matching strategy
 *
 * Each ABI has a characteristic prologue sequence.  We scan the first
 * N instructions (at most kMaxPrologueInstrs) looking for a fixed set
 * of anchor patterns:
 *
 *   PUSH RBP            — frame pointer save (SysV x64, x32)
 *   MOV  RBP, RSP       — frame pointer establish
 *   SUB  RSP/ESP, N     — frame allocation (immediate operand = frame_size)
 *   PUSH Rx             — callee-saved register spill
 *   STP  X29, X30, [SP,#-N]! — AArch64 combined save+allocate
 *   PUSH {Rx..Ry, LR}   — ARM32 callee-save list
 *
 * We stop scanning as soon as we've seen either the frame allocation or
 * the first non-prologue instruction.
 *
 * ### Frame size detection
 *
 * For x86:
 *   - "SUB RSP/ESP, N" gives frame_size = N.
 *   - If only PUSH instructions are seen (leaf functions or very small
 *     frames), frame_size = 8 × number_of_pushes (x64) or 4 × … (x32).
 *
 * For ARM64:
 *   - "STP X29, X30, [SP, #-N]!" gives frame_size = N.
 *   - Additional callee-save STP instructions increase the occupied area.
 *
 * For ARM32:
 *   - PUSH {R4–R11, LR} + optional SUB SP, SP, #N.
 *
 * ### Callee-saved register offset computation
 *
 * x86-64 SysV ABI callee-saved registers: RBX, RBP, R12-R15.
 * x86-64 Windows ABI adds: RDI, RSI.
 * x86-32: EBX, EBP, ESI, EDI.
 * AArch64: X19-X28, X29, X30.
 * ARM32: R4-R11, LR.
 *
 * For PUSH instructions in x86, the offset from RBP is computed by
 * counting how many PUSHes come before the "MOV RBP, RSP" anchor.
 * For STP instructions in AArch64, the offset is read directly from
 * the immediate.
 */

#include "retdec/var_recovery/var_recovery.h"
#include <algorithm>
#include <cstdlib>
#ifdef _MSC_VER
#  include <intrin.h>
#  pragma intrinsic(__popcnt)
static inline int __builtin_popcount(unsigned int x) { return (int)__popcnt(x); }
#endif

namespace retdec {
namespace var_recovery {

static constexpr int kMaxPrologueInstrs = 64;

// ─── x86-64 SysV prologue ─────────────────────────────────────────────────────

PrologueInfo PrologueParser::parseSysVx64(
    const std::vector<RawInstr>& instrs) const {

    PrologueInfo info;
    info.abi  = ABI::SysV_x86_64;
    info.arch = Arch::X86_64;

    int64_t  pushCount     = 0;   // PUSH instructions seen before frame alloc
    bool     seenPushRBP   = false;
    bool     seenMovRBPRSP = false;
    int64_t  subRSP        = 0;

    for (int i = 0; i < (int)instrs.size() && i < kMaxPrologueInstrs; ++i) {
        const RawInstr& ins = instrs[i];

        if (ins.op == RawInstr::Op::Push && ins.src == Reg::RBP) {
            seenPushRBP = true;
            pushCount++;
            continue;
        }
        if (ins.op == RawInstr::Op::Mov &&
            ins.dst == Reg::RBP && ins.src == Reg::RSP) {
            seenMovRBPRSP = true;
            // After MOV RBP, RSP the new frame base is established.
            // Reset pushCount so subsequent PUSH offsets are relative to RBP.
            pushCount = 0;
            continue;
        }
        if (ins.op == RawInstr::Op::Sub &&
            ins.dst == Reg::RSP && ins.hasImm) {
            subRSP = ins.imm;
            break;
        }
        // Callee-save pushes (R12-R15, RBX, etc.) after MOV RBP,RSP
        if (ins.op == RawInstr::Op::Push) {
            Reg r = ins.src;
            if (r == Reg::RBX || r == Reg::R12 || r == Reg::R13 ||
                r == Reg::R14 || r == Reg::R15) {
                // Offset from RBP: -(pushCount+1)*8
                int64_t off = -(pushCount + 1) * 8;
                info.calleeSaves.push_back({r, off});
                pushCount++;
                continue;
            }
        }
        // Non-prologue instruction → stop
        break;
    }

    if (seenPushRBP && seenMovRBPRSP) {
        info.hasFramePointer = true;
        // Saved RBP (frame chain) is at [RBP + 0]: after push RBP; mov RBP, RSP,
        // the new RBP points directly at the stored old RBP value.
        info.calleeSaves.insert(info.calleeSaves.begin(), {Reg::RBP, 0});
    }

    info.frameSize    = subRSP > 0 ? subRSP : pushCount * 8;
    info.localAreaStart = -(int64_t)info.frameSize;
    info.localAreaEnd   = 0;  // relative to RBP
    info.hasRedZone   = true;  // SysV x64 has a 128-byte red zone
    info.redZoneStart = -128;

    return info;
}

// ─── x86-64 Windows (Win64) prologue ─────────────────────────────────────────

PrologueInfo PrologueParser::parseWin64(
    const std::vector<RawInstr>& instrs) const {

    PrologueInfo info;
    info.abi  = ABI::Win64;
    info.arch = Arch::X86_64;

    int64_t  pushCount   = 0;
    bool     seenPushRBP = false;
    int64_t  subRSP      = 0;

    for (int i = 0; i < (int)instrs.size() && i < kMaxPrologueInstrs; ++i) {
        const RawInstr& ins = instrs[i];

        if (ins.op == RawInstr::Op::Sub &&
            ins.dst == Reg::RSP && ins.hasImm) {
            subRSP = ins.imm;
            break;
        }
        if (ins.op == RawInstr::Op::Push && ins.src == Reg::RBP) {
            seenPushRBP = true;
            pushCount++;
            continue;
        }
        if (ins.op == RawInstr::Op::Push) {
            Reg r = ins.src;
            // Win64 callee-saved: RBX, RBP, RDI, RSI, R12-R15
            if (r == Reg::RBX || r == Reg::RDI || r == Reg::RSI ||
                r == Reg::R12 || r == Reg::R13 || r == Reg::R14 || r == Reg::R15) {
                int64_t off = -(pushCount + 1) * 8;
                info.calleeSaves.push_back({r, off});
                pushCount++;
                continue;
            }
        }
        break;
    }

    info.hasFramePointer  = seenPushRBP;
    info.hasShadowSpace   = true;   // Win64 always has 32-byte shadow space
    info.shadowStart      = 0;      // [RSP+0..31] relative to RSP at call entry
    info.shadowSize       = 32;
    info.hasRedZone       = false;

    info.frameSize        = subRSP > 0 ? subRSP : pushCount * 8;
    info.localAreaStart   = -(int64_t)info.frameSize;
    info.localAreaEnd     = 0;

    return info;
}

// ─── x86-32 SysV (cdecl) prologue ────────────────────────────────────────────

PrologueInfo PrologueParser::parseSysVx32(
    const std::vector<RawInstr>& instrs) const {

    PrologueInfo info;
    info.abi  = ABI::SysV_x86_32;
    info.arch = Arch::X86_32;

    int64_t  pushCount     = 0;
    bool     seenPushEBP   = false;
    bool     seenMovEBPESP = false;
    int64_t  subESP        = 0;

    for (int i = 0; i < (int)instrs.size() && i < kMaxPrologueInstrs; ++i) {
        const RawInstr& ins = instrs[i];

        if (ins.op == RawInstr::Op::Push && ins.src == Reg::EBP) {
            seenPushEBP = true;
            pushCount++;
            continue;
        }
        if (ins.op == RawInstr::Op::Mov &&
            ins.dst == Reg::EBP && ins.src == Reg::ESP) {
            seenMovEBPESP = true;
            continue;
        }
        if (ins.op == RawInstr::Op::Sub &&
            ins.dst == Reg::ESP && ins.hasImm) {
            subESP = ins.imm;
            break;
        }
        if (ins.op == RawInstr::Op::Push) {
            Reg r = ins.src;
            // 32-bit callee-saved: EBX, ESI, EDI, EBP
            if (r == Reg::EBX || r == Reg::ESI || r == Reg::EDI) {
                int64_t off = -(pushCount + 1) * 4;
                info.calleeSaves.push_back({r, off});
                pushCount++;
                continue;
            }
        }
        break;
    }

    info.hasFramePointer = seenPushEBP && seenMovEBPESP;
    info.frameSize       = subESP > 0 ? subESP : pushCount * 4;
    info.localAreaStart  = -(int64_t)info.frameSize;
    info.localAreaEnd    = 0;
    info.hasRedZone      = false;
    info.hasShadowSpace  = false;

    return info;
}

// ─── AArch64 (AAPCS64) prologue ──────────────────────────────────────────────

PrologueInfo PrologueParser::parseAArch64(
    const std::vector<RawInstr>& instrs) const {

    PrologueInfo info;
    info.abi  = ABI::AAPCS64;
    info.arch = Arch::ARM64;

    for (int i = 0; i < (int)instrs.size() && i < kMaxPrologueInstrs; ++i) {
        const RawInstr& ins = instrs[i];

        // STP X29, X30, [SP, #-N]!  — canonical AArch64 prologue
        if (ins.op == RawInstr::Op::StoreRegPair &&
            ins.dst == Reg::X29 && ins.src == Reg::X30 && ins.hasImm) {
            info.frameSize     = -ins.imm; // imm is negative
            info.hasFramePointer = true;
            // X29 = frame pointer, X30 = link register
            info.calleeSaves.push_back({Reg::X29, 0});
            info.calleeSaves.push_back({Reg::X30, 8});
            continue;
        }
        // Additional callee-save STP instructions (X19-X28 pairs)
        if (ins.op == RawInstr::Op::StoreRegPair && ins.hasImm) {
            Reg lo = ins.dst;
            Reg hi = ins.src;
            int64_t off = ins.imm;
            if ((int)lo >= (int)Reg::X19 && (int)lo <= (int)Reg::X28) {
                info.calleeSaves.push_back({lo, off});
                info.calleeSaves.push_back({hi, off + 8});
                continue;
            }
        }
        if (info.frameSize > 0) break;  // stop after first STP
    }

    info.localAreaStart = -(int64_t)info.frameSize;
    info.localAreaEnd   = 0;
    info.hasRedZone     = false;
    info.hasShadowSpace = false;

    return info;
}

// ─── ARM32 (AAPCS) prologue ───────────────────────────────────────────────────

PrologueInfo PrologueParser::parseARM32(
    const std::vector<RawInstr>& instrs) const {

    PrologueInfo info;
    info.abi  = ABI::AAPCS32;
    info.arch = Arch::ARM32;

    int64_t  spAdjust     = 0;
    int64_t  calleeSaveSize = 0;

    for (int i = 0; i < (int)instrs.size() && i < kMaxPrologueInstrs; ++i) {
        const RawInstr& ins = instrs[i];

        // PUSH {R4-R11, LR} encoded as PushList with bitmask in imm
        if (ins.op == RawInstr::Op::PushList) {
            // Count saved registers from the bitmask
            uint32_t mask = (uint32_t)ins.imm;
            int count = __builtin_popcount(mask);
            calleeSaveSize = count * 4;
            // Record individual registers
            int offset = 0;
            for (int bit = 0; bit < 16; ++bit) {
                if (mask & (1u << bit)) {
                    Reg r = static_cast<Reg>((int)Reg::R0 + bit);
                    info.calleeSaves.push_back({r, offset});
                    offset += 4;
                }
            }
            info.hasFramePointer = (mask & (1u << 11)) != 0; // R11 = fp
            continue;
        }
        // SUB SP, SP, #N  — local frame allocation
        if (ins.op == RawInstr::Op::Sub &&
            ins.dst == Reg::SP_ARM32 && ins.hasImm) {
            spAdjust = ins.imm;
            break;
        }
    }

    info.frameSize     = calleeSaveSize + spAdjust;
    info.localAreaStart= -(int64_t)spAdjust;
    info.localAreaEnd  = 0;
    info.hasRedZone    = false;
    info.hasShadowSpace= false;

    return info;
}

// ─── Dispatch ─────────────────────────────────────────────────────────────────

PrologueInfo PrologueParser::parse(const std::vector<RawInstr>& instrs) const {
    switch (abi_) {
    case ABI::SysV_x86_64: return parseSysVx64(instrs);
    case ABI::Win64:        return parseWin64(instrs);
    case ABI::Win32:        [[fallthrough]];
    case ABI::SysV_x86_32: return parseSysVx32(instrs);
    case ABI::AAPCS64:      return parseAArch64(instrs);
    case ABI::AAPCS32:      return parseARM32(instrs);
    default:
        // Return an invalid info for unknown ABI
        return PrologueInfo{};
    }
}

// ─── Register name table ─────────────────────────────────────────────────────

const char* regName(Reg r) noexcept {
    switch (r) {
    case Reg::RAX: return "rax"; case Reg::RCX: return "rcx";
    case Reg::RDX: return "rdx"; case Reg::RBX: return "rbx";
    case Reg::RSP: return "rsp"; case Reg::RBP: return "rbp";
    case Reg::RSI: return "rsi"; case Reg::RDI: return "rdi";
    case Reg::R8:  return "r8";  case Reg::R9:  return "r9";
    case Reg::R10: return "r10"; case Reg::R11: return "r11";
    case Reg::R12: return "r12"; case Reg::R13: return "r13";
    case Reg::R14: return "r14"; case Reg::R15: return "r15";
    case Reg::EAX: return "eax"; case Reg::ECX: return "ecx";
    case Reg::EDX: return "edx"; case Reg::EBX: return "ebx";
    case Reg::ESP: return "esp"; case Reg::EBP: return "ebp";
    case Reg::ESI: return "esi"; case Reg::EDI: return "edi";
    case Reg::X29: return "x29"; case Reg::X30: return "x30";
    case Reg::SP_ARM64: return "sp";
    case Reg::R0:  return "r0";  case Reg::R1:  return "r1";
    case Reg::R2:  return "r2";  case Reg::R3:  return "r3";
    case Reg::R4:  return "r4";  case Reg::R5:  return "r5";
    case Reg::LR:  return "lr";  case Reg::PC:  return "pc";
    default: return "?";
    }
}

} // namespace var_recovery
} // namespace retdec
