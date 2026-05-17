/**
 * @file include/retdec/call_conv/call_conv.h
 * @brief ABI-Constrained Calling Convention Detection (Stage 21).
 *
 * ## Overview
 *
 * This module determines the calling convention, argument list, return type,
 * and variadic nature of each decompiled function by observing machine-level
 * ABI evidence.  The output is a `CallingConvention` descriptor that drives
 * the C code generator (Stage 24) to emit correct function signatures.
 *
 * ## Detection sub-passes
 *
 * ### 1. Caller/Callee Stack Cleanup Detection (x86-32 only)
 *
 * On x86-32, the distinction between `cdecl` (caller cleans up) and
 * `stdcall`/`thiscall`/`fastcall` (callee cleans up) is structural:
 *
 *   cdecl:    CALL foo  →  ADD ESP, N        (caller removes N bytes)
 *   stdcall:  CALL foo  →  (no ADD ESP)       (callee used RET N)
 *   fastcall: first 1-2 args in ECX[,EDX], rest on stack, callee cleans.
 *   thiscall: ECX = this, rest on stack, callee cleans.
 *
 * Algorithm:
 *   At each call site (IrInstr::Op::Call), inspect the instruction immediately
 *   following the call in the same basic block.  If it is an ADD with ESP as
 *   the destination and an immediate operand → cdecl, bytes = imm.
 *   Otherwise → callee-cleanup (stdcall/thiscall/fastcall).
 *
 *   The majority-vote over all call sites in the binary determines the
 *   convention for each callee.
 *
 * ### 2. Register Argument Liveness Analysis (x86-64)
 *
 * ABI register sets for integer/pointer arguments:
 *
 *   Linux SysV AMD64:  RDI, RSI, RDX, RCX, R8, R9  (+ XMM0-XMM7 for float)
 *   Windows x64:       RCX, RDX, R8, R9             (+ XMM0-XMM3 for float)
 *   AArch64 SysV:      X0-X7  (+ V0-V7 for float)
 *   ARM32 AAPCS:       R0-R3  (no float register args in AAPCS soft-float)
 *
 * At function entry, we examine the liveness analysis results from the SSA
 * module (BasicBlock::liveIn of the entry block).  The highest-indexed ABI
 * register that is live-in gives the argument count.
 *
 * We also check for "holes" (skipped registers), which indicate that an
 * earlier argument was passed on the stack or that the register was used for
 * something else — rare in compiler-generated code.
 *
 * ### 3. Return Value Analysis
 *
 * At every RET instruction, we inspect which return registers are live-out:
 *
 *   SysV x64:  integer in RAX; 128-bit in RAX:RDX; float in XMM0.
 *   Win64:     integer in RAX; float in XMM0.
 *   AArch64:   X0; float in V0.
 *   ARM32:     R0; R0:R1 for 64-bit; float in S0/D0.
 *
 * If no return registers are live → void return.
 * If XMM0 is live and RAX is not → float/double return.
 * If RAX and RDX both live → 64-bit integer (SysV) or struct-in-registers.
 *
 * ### 4. Variadic Function Detection
 *
 * Linux SysV AMD64:
 *   If AL is live-in at function entry, the function is variadic.
 *   (AL holds the count of XMM arguments for va_start on SysV.)
 *   Alternatively, accessing the argument save area [RBP - offset_to_reg_save]
 *   indicates variadic.
 *
 * Windows x64:
 *   va_list is a pointer; the compiler generates a pattern of
 *   `LEA va_ptr, [RBP+offset]` where offset points to the shadow space.
 *   We detect this by finding a LEA into a stack slot used only for
 *   sequential pointer arithmetic (va_arg pattern).
 *
 * x86-32 cdecl:
 *   A function that accesses stack arguments beyond a fixed frame size
 *   (past all discovered named args) may be variadic.
 *
 * ## Output: CallingConvention
 *
 *   enum class CC { Cdecl, Stdcall, Fastcall, Thiscall, SysVAmd64,
 *                   Win64, AArch64SysV, Arm32Aapcs, Unknown }
 *
 *   struct ArgDesc  { ArgKind kind; PhysReg reg; int32_t stackOffset;
 *                     uint8_t width; bool isFp; }
 *
 *   struct RetDesc  { RetKind kind; std::vector<PhysReg> regs; uint8_t width; }
 *
 *   struct CallingConvention { CC cc; std::vector<ArgDesc> args;
 *                              RetDesc ret; bool isVariadic; }
 */

#ifndef RETDEC_CALL_CONV_H
#define RETDEC_CALL_CONV_H

#include <cstdint>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace retdec {
namespace ssa {
class SSAFunction;
struct IrInstr;
} // namespace ssa
} // namespace retdec

namespace retdec {
namespace call_conv {

// ─── Physical register IDs (architecture-neutral) ────────────────────────────

enum class PhysReg : uint16_t {
    // x86-64 general purpose
    RAX = 0, RCX, RDX, RBX, RSP, RBP, RSI, RDI,
    R8,  R9,  R10, R11, R12, R13, R14, R15,
    // x86-32 (re-use lower IDs, distinguished by context)
    EAX = 0, ECX = 1, EDX = 2, EBX = 3, ESP = 4, EBP = 5, ESI = 6, EDI = 7,
    // Sub-registers
    AL  = 100,
    // SSE/AVX
    XMM0 = 200, XMM1, XMM2, XMM3, XMM4, XMM5, XMM6, XMM7,
    // AArch64 general purpose
    X0  = 300, X1, X2, X3, X4, X5, X6, X7,
    // AArch64 float
    V0  = 400, V1, V2, V3, V4, V5, V6, V7,
    // ARM32
    R0  = 500, R1, R2, R3,
    // ARM32 float
    S0  = 600, D0 = 601,
    // Sentinel
    Invalid = 0xFFFF
};

inline const char* physRegName(PhysReg r) noexcept {
    switch (r) {
    case PhysReg::RAX: return "rax";  case PhysReg::RCX: return "rcx";
    case PhysReg::RDX: return "rdx";  case PhysReg::RBX: return "rbx";
    case PhysReg::RSP: return "rsp";  case PhysReg::RBP: return "rbp";
    case PhysReg::RSI: return "rsi";  case PhysReg::RDI: return "rdi";
    case PhysReg::R8:  return "r8";   case PhysReg::R9:  return "r9";
    case PhysReg::R10: return "r10";  case PhysReg::R11: return "r11";
    case PhysReg::R12: return "r12";  case PhysReg::R13: return "r13";
    case PhysReg::R14: return "r14";  case PhysReg::R15: return "r15";
    case PhysReg::AL:  return "al";
    case PhysReg::XMM0: return "xmm0"; case PhysReg::XMM1: return "xmm1";
    case PhysReg::XMM2: return "xmm2"; case PhysReg::XMM3: return "xmm3";
    case PhysReg::XMM4: return "xmm4"; case PhysReg::XMM5: return "xmm5";
    case PhysReg::XMM6: return "xmm6"; case PhysReg::XMM7: return "xmm7";
    case PhysReg::X0: return "x0"; case PhysReg::X1: return "x1";
    case PhysReg::X2: return "x2"; case PhysReg::X3: return "x3";
    case PhysReg::X4: return "x4"; case PhysReg::X5: return "x5";
    case PhysReg::X6: return "x6"; case PhysReg::X7: return "x7";
    case PhysReg::V0: return "v0"; case PhysReg::V1: return "v1";
    case PhysReg::V2: return "v2"; case PhysReg::V3: return "v3";
    case PhysReg::V4: return "v4"; case PhysReg::V5: return "v5";
    case PhysReg::V6: return "v6"; case PhysReg::V7: return "v7";
    case PhysReg::R0: return "r0"; case PhysReg::R1: return "r1";
    case PhysReg::R2: return "r2"; case PhysReg::R3: return "r3";
    case PhysReg::S0: return "s0"; case PhysReg::D0: return "d0";
    default: return "?reg";
    }
}

// ─── Calling convention enum ──────────────────────────────────────────────────

enum class CC : uint8_t {
    Unknown,
    // x86-32
    Cdecl,      ///< Caller cleans up; args right-to-left on stack
    Stdcall,    ///< Callee cleans up; args right-to-left on stack
    Fastcall,   ///< ECX, EDX first args; rest on stack; callee cleans
    Thiscall,   ///< ECX = this; rest on stack; callee cleans
    // x86-64
    SysVAmd64,  ///< Linux/macOS: RDI,RSI,RDX,RCX,R8,R9 + XMM0-7
    Win64,      ///< Windows: RCX,RDX,R8,R9 + XMM0-3
    // ARM
    AArch64SysV,///< X0-X7 + V0-V7
    Arm32Aapcs, ///< R0-R3 (soft-float or VFP)
};

inline const char* ccName(CC c) noexcept {
    switch (c) {
    case CC::Cdecl:       return "cdecl";
    case CC::Stdcall:     return "stdcall";
    case CC::Fastcall:    return "fastcall";
    case CC::Thiscall:    return "thiscall";
    case CC::SysVAmd64:   return "sysv_amd64";
    case CC::Win64:       return "win64";
    case CC::AArch64SysV: return "aarch64_sysv";
    case CC::Arm32Aapcs:  return "arm32_aapcs";
    default:              return "unknown";
    }
}

// ─── Argument descriptor ──────────────────────────────────────────────────────

enum class ArgKind : uint8_t {
    Register,   ///< Passed in a physical register
    Stack,      ///< Passed on the stack at a given offset from SP/BP
};

struct ArgDesc {
    ArgKind  kind        = ArgKind::Register;
    PhysReg  reg         = PhysReg::Invalid;
    int32_t  stackOffset = 0;      ///< byte offset from stack pointer at call
    uint8_t  width       = 64;     ///< bit width
    bool     isFp        = false;  ///< floating-point argument
    uint32_t ssaValueId  = UINT32_MAX; ///< matching SSA value at entry (if known)

    std::string toString() const;
};

// ─── Return value descriptor ──────────────────────────────────────────────────

enum class RetKind : uint8_t {
    Void,
    Integer,    ///< RAX (or RAX:RDX for 128-bit)
    Float,      ///< XMM0 / S0 / D0 / V0
    Struct,     ///< RAX:RDX or hidden-pointer
};

struct RetDesc {
    RetKind             kind  = RetKind::Void;
    std::vector<PhysReg> regs;   ///< physical registers holding the return value
    uint8_t             width = 0;
    bool                isFp  = false;

    std::string toString() const;
};

// ─── Full calling convention descriptor ──────────────────────────────────────

struct CallingConvention {
    CC                   cc         = CC::Unknown;
    std::vector<ArgDesc> args;
    RetDesc              ret;
    bool                 isVariadic = false;
    int32_t              stackCleanupBytes = 0; ///< bytes popped by callee (stdcall)

    std::string toString() const;
};

// ─── ABI register tables ──────────────────────────────────────────────────────

/// Returns the ordered list of integer/pointer argument registers for a CC.
std::vector<PhysReg> intArgRegs(CC cc);

/// Returns the ordered list of float/SSE argument registers for a CC.
std::vector<PhysReg> fpArgRegs(CC cc);

/// Returns the integer return registers for a CC.
std::vector<PhysReg> intRetRegs(CC cc);

/// Returns the float return register(s) for a CC.
std::vector<PhysReg> fpRetRegs(CC cc);

// ─── Caller/Callee cleanup detector (x86-32) ─────────────────────────────────

/**
 * Examines each call site in the function.  If the instruction immediately
 * after a CALL is `ADD ESP, N`, records a caller-cleanup vote with N bytes.
 * Otherwise records a callee-cleanup vote.
 *
 * The majority vote determines the convention; ties → callee-cleanup.
 *
 * Also detects `fastcall` (caller uses ECX and possibly EDX as first args)
 * and `thiscall` (caller puts a pointer in ECX before the call).
 */
class CallerCleanupDetector {
public:
    struct CallSiteEvidence {
        uint32_t callInstrId   = UINT32_MAX;
        bool     callerCleanup = false;
        int32_t  cleanupBytes  = 0;
        bool     ecxUsed       = false;
        bool     edxUsed       = false;
    };

    struct Result {
        CC      cc              = CC::Unknown;
        int32_t majorityCleanup = 0;  ///< most common cleanup size
        std::vector<CallSiteEvidence> sites;
        int callerCleanupVotes = 0;
        int calleeCleanupVotes = 0;
    };

    Result run(const ssa::SSAFunction& fn) const;
};

// ─── Register argument liveness analysis ─────────────────────────────────────

/**
 * Determines how many (and which) ABI argument registers are live-in at the
 * function entry block, for the given assumed calling convention.
 *
 * Liveness data comes from `BasicBlock::liveIn` of the entry block.
 *
 * We map each physical ABI register to its corresponding SSA VarId by
 * scanning entry-block phi nodes and instructions that define those registers.
 *
 * Output: an ordered list of `ArgDesc` — one per live argument register,
 * in ABI order.  The list is truncated at the first non-live register
 * (no holes assumed for compiler-generated code).
 */
class RegArgAnalysis {
public:
    std::vector<ArgDesc> run(const ssa::SSAFunction& fn, CC cc) const;

private:
    // Maps physical register name → SSA VarId via the function's variable table.
    uint32_t findVarForReg(const ssa::SSAFunction& fn, PhysReg r) const;
};

// ─── Return value analysis ────────────────────────────────────────────────────

/**
 * For each RET instruction in the function, checks which return-value
 * registers are live-out at that point.
 *
 * Uses `BasicBlock::liveOut` of blocks ending in IrInstr::Op::Ret.
 */
class ReturnValueAnalysis {
public:
    RetDesc run(const ssa::SSAFunction& fn, CC cc) const;
};

// ─── Variadic function detection ─────────────────────────────────────────────

/**
 * Determines whether the function is variadic:
 *
 *  SysV AMD64:   AL live-in at entry block.
 *  Win64:        LEA pattern into shadow-space pointer.
 *  x86-32 cdecl: stack access beyond highest named argument.
 *  AArch64:      x-registers beyond X7 used as if spilled args.
 */
class VariadicDetector {
public:
    bool run(const ssa::SSAFunction& fn, CC cc) const;

private:
    bool checkSysVAl(const ssa::SSAFunction& fn) const;
    bool checkWin64VaList(const ssa::SSAFunction& fn) const;
    bool checkX86CdeclExtendedStack(const ssa::SSAFunction& fn,
                                     int numNamedArgs) const;
};

// ─── Main calling convention pass ────────────────────────────────────────────

/**
 * Orchestrates all four sub-passes and produces a `CallingConvention` for
 * each function.
 *
 * The platform hint (CC) is supplied by the caller (usually derived from
 * the binary's ELF/PE header and the target architecture detected during
 * loading).  The pass refines it using evidence from the function body.
 */
class CallConvPass {
public:
    struct Config {
        CC    platformCC = CC::SysVAmd64; ///< initial platform hint
        bool  is32bit    = false;
        bool  allowThiscall = true;
        bool  allowFastcall = true;
    };
    static Config defaultConfig() noexcept { return {}; }

    struct Stats {
        std::size_t cdeclFunctions      = 0;
        std::size_t stdcallFunctions    = 0;
        std::size_t fastcallFunctions   = 0;
        std::size_t thiscallFunctions   = 0;
        std::size_t sysvFunctions       = 0;
        std::size_t win64Functions      = 0;
        std::size_t variadicFunctions   = 0;
        std::size_t unknownFunctions    = 0;
        std::size_t totalFunctions      = 0;
    };

    CallingConvention run(const ssa::SSAFunction& fn,
                           const Config& cfg = defaultConfig()) const;

    // Batch version for a whole-module map of functions.
    std::unordered_map<std::string, CallingConvention>
    runAll(const std::vector<const ssa::SSAFunction*>& fns,
           const Config& cfg = defaultConfig()) const;

    const Stats& stats() const { return stats_; }

private:
    mutable Stats stats_;
    void accumStats(CC cc, bool variadic) const;
};

} // namespace call_conv
} // namespace retdec

#endif // RETDEC_CALL_CONV_H
