/**
 * @file include/retdec/var_recovery/var_recovery.h
 * @brief ABI-Aware Variable Recovery with DVSA (Data-flow-driven Variable
 *        and Stack-slot Analysis).
 *
 * ## Overview
 *
 * Variable recovery bridges the gap between "named registers and raw memory
 * accesses" and "named source-level variables".  It operates on an SSA-form
 * function (produced by Stage 16) and produces a set of VariableCandidate
 * objects that can be used by the type-inference and code-generation stages.
 *
 * The process has four sequential sub-stages:
 *
 * ### Sub-stage A: Prologue/epilogue parsing
 *
 * Detects the compiler-generated function frame setup:
 *
 *   x86-64 System V (Linux/macOS):
 *     PUSH RBP
 *     MOV  RBP, RSP
 *     SUB  RSP, N          ← frame_size
 *     PUSH R12, R13, ...   ← callee-saved registers
 *
 *   x86-64 Windows (MSVC/MinGW):
 *     PUSH RBP (optional)
 *     SUB  RSP, N          ← frame_size (must be 16-byte aligned after call)
 *     MOV  [RSP+0..31], RCX/RDX/R8/R9  ← shadow store (spill space)
 *     PUSH R12-R15, RDI, RSI, RBX       ← callee-saved
 *
 *   x86 (32-bit):
 *     PUSH EBP
 *     MOV  EBP, ESP
 *     SUB  ESP, N
 *
 *   ARM64 AArch64 (AAPCS64):
 *     STP  X29, X30, [SP, #-frameSize]!
 *     MOV  X29, SP
 *     STP  X19..X28 at subsequent frame offsets
 *
 *   ARM32 (AAPCS):
 *     PUSH {R4-R11, LR}
 *     SUB  SP, SP, #localSize
 *
 * ### Sub-stage B: ABI region carving
 *
 * After the prologue is parsed, known ABI-mandated frame regions are
 * carved out and excluded from the DVSA variable search:
 *
 *   return_address  — at [RBP+8] on x86-64 / [EBP+4] on x86-32
 *   callee_saves    — the pushed callee-saved register values
 *   shadow_space    — Windows: [RSP+0..31] (home space for first 4 args)
 *   red_zone        — Linux x86-64: [RSP-128..RSP-1] (not safe for alloca)
 *   frame_chain     — the saved RBP/EBP value at [RBP+0]
 *
 * ### Sub-stage C: DVSA — Data-flow-driven Variable and Stack-slot Analysis
 *
 * Scans all memory accesses (`MemRef` values in the SSA IR) within the
 * remaining (non-carved) frame area and collects (offset, access_size) pairs
 * for each unique SP-relative or BP-relative address.
 *
 * Non-overlap partitioning:
 *   Sort collected accesses by base offset.  Walk them in order; whenever two
 *   consecutive accesses do not overlap ([off_i, off_i+size_i) ∩
 *   [off_j, off_j+size_j) = ∅), they belong to distinct variables.  Each
 *   non-overlapping slot becomes one VariableCandidate.
 *
 * Overlap handling (two cases):
 *   1. Lifetimes don't overlap (confirmed by SSA liveness): variable reuse.
 *      Create two separate VariableCandidates with distinct SSA names.
 *      Example: sub-word access to the lower bytes of a wider slot.
 *   2. Lifetimes do overlap: the slot is genuinely used with multiple types.
 *      Emit a union candidate (`isUnion = true`).  Code generation will
 *      produce a C anonymous union for this slot.
 *      Example: endian-conversion idiom that writes 4 bytes then reads 1.
 *
 * ### Sub-stage D: Variable naming
 *
 * Assigns human-readable names to each VariableCandidate:
 *   - If a DWARF debug-info entry covers the slot, use the DWARF name.
 *   - Otherwise: `<type_prefix>_<index>`, where type_prefix is inferred
 *     from the access width: `b` (byte), `w` (word), `d` (dword),
 *     `q` (qword), `ptr` (pointer-width), `v` (vector).
 *   - Numeric suffix is per-type, resetting per function.
 *
 * ## Data structures
 *
 *   FrameAccess   — a single (offset, size, isWrite) observation
 *   FrameSlot     — a non-overlap partition result: [base, base+size)
 *   FrameRegion   — an ABI-reserved region (excluded from DVSA)
 *   PrologueInfo  — full parsed prologue: frame_size, saved regs, ABI flags
 *   VariableCandidate — recovered variable: slot, name, SSA value IDs,
 *                        isUnion, isCalleeSave, isArg, isDwarfNamed
 *
 * ## Integration
 *
 * VarRecoveryPass takes an SSAFunction (from Stage 16) and an optional
 * DWARF info table, and annotates the function with VariableCandidate objects.
 * Later stages (type inference, code gen) consume this information.
 */

#ifndef RETDEC_VAR_RECOVERY_H
#define RETDEC_VAR_RECOVERY_H

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace retdec {
namespace ssa {
class SSAFunction;
} // namespace ssa
} // namespace retdec

namespace retdec {
namespace var_recovery {

// ─── ABI / calling convention identifiers ────────────────────────────────────

enum class ABI : uint8_t {
    Unknown,
    SysV_x86_64,    ///< Linux/macOS 64-bit
    Win64,          ///< Windows x64 (MSVC, MinGW)
    SysV_x86_32,    ///< Linux 32-bit cdecl
    Win32,          ///< Windows 32-bit (MSVC, BCC, DMC, Watcom)
    AAPCS64,        ///< ARM64 / AArch64
    AAPCS32,        ///< ARM32 / Thumb
    Watcom_x86,     ///< Open Watcom register-calling convention
};

enum class Arch : uint8_t {
    Unknown,
    X86_32,
    X86_64,
    ARM32,
    ARM64,
};

// ─── Frame access observation ────────────────────────────────────────────────

/**
 * One observed stack-frame memory access.
 * Offset is relative to the frame base (RBP / EBP / SP+frame_size).
 * Negative offsets are local variables; positive offsets are arguments.
 */
struct FrameAccess {
    int64_t  offset   = 0;     ///< signed offset from frame base
    uint8_t  size     = 0;     ///< access width in bytes
    bool     isWrite  = false; ///< store vs load
    uint64_t vma      = 0;     ///< instruction address (for debug)
    uint32_t ssaValue = UINT32_MAX; ///< SSA ValueId of the MemRef
};

// ─── Frame slot (DVSA partition result) ──────────────────────────────────────

/**
 * One non-overlapping variable slot in the frame.
 * Produced by the DVSA partitioner.
 */
struct FrameSlot {
    int64_t  baseOffset = 0;   ///< lowest byte offset of the slot
    uint8_t  totalSize  = 0;   ///< byte span of the slot
    uint8_t  maxAccess  = 0;   ///< widest single access seen (for type hint)
    bool     hasWrite   = false;
    bool     hasRead    = false;
    std::vector<FrameAccess> accesses;  ///< all accesses to this slot
};

// ─── ABI-reserved frame regions ──────────────────────────────────────────────

enum class RegionKind : uint8_t {
    ReturnAddress,
    CalleeSave,
    ShadowSpace,    ///< Windows: home area for first 4 arguments
    RedZone,        ///< System V: 128-byte scratch below RSP
    FrameChain,     ///< Saved RBP/EBP
    Argument,       ///< Stack-passed argument (above frame base)
};

struct FrameRegion {
    RegionKind kind;
    int64_t    offset = 0;  ///< relative to frame base
    uint8_t    size   = 0;  ///< in bytes
    std::string name;       ///< human-readable (e.g. "arg0", "saved_r12")
};

// ─── Register identifiers for prologue parsing ───────────────────────────────

enum class Reg : uint8_t {
    // x86-64
    RAX=0, RCX, RDX, RBX, RSP, RBP, RSI, RDI,
    R8, R9, R10, R11, R12, R13, R14, R15,
    // x86-32
    EAX=20, ECX, EDX, EBX, ESP, EBP, ESI, EDI,
    // ARM64
    X0=40, X1, X2, X3, X4, X5, X6, X7,
    X8, X9, X10, X11, X12, X13, X14, X15,
    X16, X17, X18, X19, X20, X21, X22, X23,
    X24, X25, X26, X27, X28, X29, X30,
    SP_ARM64=80,
    // ARM32
    R0=90, R1, R2, R3, R4, R5, R6, R7,
    R8_ARM32, R9_ARM32, R10_ARM32, R11_ARM32, R12_ARM32, SP_ARM32, LR, PC,
    // Sentinels
    None = 0xFF,
};

const char* regName(Reg r) noexcept;

// ─── Prologue information ─────────────────────────────────────────────────────

/**
 * Result of prologue parsing for one function.
 */
struct PrologueInfo {
    ABI   abi            = ABI::Unknown;
    Arch  arch           = Arch::Unknown;
    bool  hasFramePointer= false;   ///< RBP/EBP/X29 set up as frame pointer
    bool  hasRedZone     = false;   ///< Only for SysV_x86_64
    bool  hasShadowSpace = false;   ///< Only for Win64

    int64_t  frameSize    = 0;      ///< total frame allocation (positive)
    int64_t  localAreaStart= 0;     ///< first byte below frame pointer for locals
    int64_t  localAreaEnd  = 0;     ///< last  byte (exclusive) of local area
    int64_t  redZoneStart  = -128;  ///< only valid if hasRedZone
    int64_t  shadowStart   = 0;     ///< only valid if hasShadowSpace
    uint8_t  shadowSize    = 32;    ///< always 32 for Win64

    std::vector<std::pair<Reg, int64_t>> calleeSaves; ///< (reg, frame_offset)

    /// All carved ABI regions (pre-filled by ABI region carver).
    std::vector<FrameRegion> abiRegions;

    bool isValid() const { return arch != Arch::Unknown; }
};

// ─── DWARF variable info (thin wrapper; full DWARF is in a separate module) ──

struct DwarfVarInfo {
    std::string name;
    int64_t     frameOffset = 0;  ///< DW_AT_location (CFA-relative)
    uint8_t     size        = 0;  ///< DW_AT_byte_size
};

// ─── Variable candidate ───────────────────────────────────────────────────────

/**
 * One recovered variable candidate.  After type inference this gains a
 * full type; after naming it gets a final identifier.
 */
struct VariableCandidate {
    uint32_t    id          = 0;
    std::string name;           ///< assigned by VariableNamer
    FrameSlot   slot;

    // Classification flags
    bool isCalleeSave  = false; ///< saved callee-saved register value
    bool isArg         = false; ///< stack-passed argument (above RBP)
    bool isReturn      = false; ///< return value slot
    bool isUnion       = false; ///< overlapping lifetimes → C union
    bool isDwarfNamed  = false; ///< name came from DWARF debug info

    // SSA value IDs that reference this variable (for type seeding)
    std::vector<uint32_t> ssaValueIds;

    // For union candidates, the individual access sub-slots
    std::vector<FrameSlot> unionMembers;
};

// ─── Prologue parser ──────────────────────────────────────────────────────────

/**
 * Parses the function prologue from decoded instructions to extract
 * frame layout information.
 *
 * The parser recognises patterns for x86-64 (SysV and Win64), x86-32,
 * ARM64 (AArch64), and ARM32.
 *
 * Input: a sequence of (opcode, operands) in architecture-neutral form.
 * For integration with the full decoder this is abstracted as a
 * `RawInstrSeq` (a vector of `RawInstr`).
 */
struct RawInstr {
    enum class Op : uint8_t {
        Push, Pop, Sub, Add, Mov, Lea,
        StoreRegPair, // STP (ARM64)
        PushList,     // PUSH {r4-r11, lr} (ARM32)
        Other,
    };
    Op       op   = Op::Other;
    Reg      dst  = Reg::None;
    Reg      src  = Reg::None;
    int64_t  imm  = 0;          ///< immediate or offset
    bool     hasImm = false;
    uint64_t vma  = 0;
};

class PrologueParser {
public:
    explicit PrologueParser(ABI abi, Arch arch)
        : abi_(abi), arch_(arch) {}

    PrologueInfo parse(const std::vector<RawInstr>& instrs) const;

private:
    PrologueInfo parseSysVx64(const std::vector<RawInstr>& instrs) const;
    PrologueInfo parseWin64(const std::vector<RawInstr>& instrs) const;
    PrologueInfo parseSysVx32(const std::vector<RawInstr>& instrs) const;
    PrologueInfo parseAArch64(const std::vector<RawInstr>& instrs) const;
    PrologueInfo parseARM32(const std::vector<RawInstr>& instrs) const;

    ABI  abi_;
    Arch arch_;
};

// ─── ABI region carver ────────────────────────────────────────────────────────

/**
 * Fills PrologueInfo::abiRegions based on the parsed frame layout and ABI.
 */
class AbiRegionCarver {
public:
    void carve(PrologueInfo& info) const;

private:
    void carveSysVx64(PrologueInfo& info) const;
    void carveWin64(PrologueInfo& info) const;
    void carveSysVx32(PrologueInfo& info) const;
    void carveAArch64(PrologueInfo& info) const;
    void carveARM32(PrologueInfo& info) const;

public:
    bool isCarved(const PrologueInfo& info, int64_t off, uint8_t size) const;
};

// ─── DVSA (Data-flow Variable and Stack-slot Analysis) ────────────────────────

/**
 * Collects all MemRef accesses from the SSA IR, excludes ABI-reserved
 * regions, then partitions the remainder into non-overlapping FrameSlots.
 *
 * Overlap policy:
 *   - Lifetime check: uses SSA def-use chains.  If no two live ranges of
 *     overlapping accesses are simultaneously live, they are split into
 *     separate candidates (variable reuse).
 *   - Otherwise: merged into a union candidate.
 */
class DVSA {
public:
    struct Result {
        std::vector<FrameSlot> slots;
        std::vector<FrameSlot> unionSlots;  ///< slots with overlapping lifetimes
        std::size_t totalAccesses  = 0;
        std::size_t carvedAccesses = 0;     ///< excluded as ABI-reserved
    };

    Result run(const ssa::SSAFunction& fn,
               const PrologueInfo& prologue) const;

private:
    std::vector<FrameAccess> collectAccesses(
        const ssa::SSAFunction& fn,
        const PrologueInfo& prologue) const;

    std::vector<FrameSlot> partition(
        std::vector<FrameAccess>& accesses) const;

    bool accessesOverlap(const FrameAccess& a, const FrameAccess& b) const;
    bool isCarved(const PrologueInfo& info,
                  int64_t offset, uint8_t size) const;
};

// ─── Variable namer ───────────────────────────────────────────────────────────

/**
 * Assigns human-readable names to recovered variable candidates.
 *
 * Naming priority:
 *   1. DWARF DW_AT_name if offset matches within ±1 byte.
 *   2. Argument names from calling-convention position (arg0, arg1, ...).
 *   3. Callee-save names (saved_rbx, saved_r12, ...).
 *   4. Type-prefixed auto name: b0, w1, d2, q3, ptr4, v5.
 */
class VariableNamer {
public:
    void name(std::vector<VariableCandidate>& candidates,
              const PrologueInfo& prologue,
              const std::vector<DwarfVarInfo>& dwarf = {}) const;

private:
    std::string autoName(const FrameSlot& slot, uint32_t counters[6]) const;
    const char* typePrefix(uint8_t accessWidth) const;
};

// ─── Main variable recovery pass ─────────────────────────────────────────────

/**
 * Orchestrates:
 *   1. PrologueParser  — extract frame layout
 *   2. AbiRegionCarver — mark ABI-reserved regions
 *   3. DVSA            — partition remaining frame into slots
 *   4. VariableNamer   — assign names
 *
 * Produces a flat list of VariableCandidate objects that annotate the
 * SSAFunction.  The function itself is not modified; callers attach
 * the result as metadata.
 */
class VarRecoveryPass {
public:
    struct Config {
        ABI   abi   = ABI::Unknown;
        Arch  arch  = Arch::Unknown;
        std::vector<DwarfVarInfo> dwarf;  ///< optional debug info
    };

    struct Result {
        std::vector<VariableCandidate> candidates;
        PrologueInfo                   prologue;
        DVSA::Result                   dvsaStats;
        std::size_t                    dwarfMatchCount = 0;
    };

    Result run(const ssa::SSAFunction& fn,
               const std::vector<RawInstr>& prologueInstrs,
               const Config& cfg) const;
};

} // namespace var_recovery
} // namespace retdec

#endif // RETDEC_VAR_RECOVERY_H
