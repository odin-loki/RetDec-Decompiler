/**
 * @file include/retdec/dce/dce.h
 * @brief ABI-Filtered Dead Code Elimination (Stage 22).
 *
 * ## Overview
 *
 * Standard compiler DCE eliminates code that produces values unused by any
 * subsequent instruction.  For *decompilation*, this is insufficient: the
 * decompiler must additionally understand which machine-level operations are
 * ABI artifacts — bookkeeping that the compiler inserted for calling-convention
 * correctness but that has no equivalent at the C source level.
 *
 * This module performs a three-phase elimination:
 *
 * ### Phase 1 — ABI Artifact Pre-Marking
 *
 * Before any liveness analysis, we identify instructions that are definitively
 * ABI bookkeeping and can be removed regardless of data-flow:
 *
 *   **Stack alignment:**
 *     `AND RSP, -16` (and similar AND RSP,-N) immediately after function
 *     entry is a pure ABI artifact.  The C source never contains this
 *     operation; the compiler inserts it to satisfy SSE/AVX alignment.
 *     Mark the AND instruction as `AbiArtifact::StackAlign`.
 *
 *   **Shadow space:**
 *     On Win64, the callee's first 32 bytes above the return address
 *     ([RSP+8]..[RSP+40]) are the "shadow space" (home space) for RCX/RDX/R8/R9.
 *     If the function itself never writes to these slots, any read from them
 *     is a dead ABI artefact.
 *     Mark such reads as `AbiArtifact::ShadowSpaceRead`.
 *
 *   **Callee-save register save/restore pairs:**
 *     Compilers save callee-save registers (RBX, RBP, R12-R15 on SysV; RBX,
 *     RBP, RDI, RSI, R12-R15, XMM6-XMM15 on Win64) at the function prologue
 *     and restore them in the epilogue.  These pairs are invisible to the C
 *     programmer.
 *
 *     Detection: find `MOV [RBP±offset], Rx` (or PUSH Rx) in the prologue
 *     paired with `MOV Rx, [RBP±offset]` (or POP Rx) in the epilogue.
 *     A save/restore pair is "balanced" if:
 *       - Every path from the save to a function exit passes through the
 *         corresponding restore.
 *       - The saved value is not modified between save and restore.
 *     Balanced pairs are marked `AbiArtifact::CalleeSavePair`.
 *
 *   **Red zone:**
 *     On SysV AMD64, the 128 bytes below RSP are the "red zone" — the compiler
 *     may use them as scratch without adjusting RSP.  Any store to [RSP-N]
 *     for N ∈ [1,128] that is never read after the function returns is marked
 *     `AbiArtifact::RedZoneAccess`.
 *
 * ### Phase 2 — C-Level Liveness Propagation
 *
 * Live roots at the C semantic level (instructions that *must* produce
 * observable effects):
 *
 *   - **Return values:** instructions that write to return-value registers
 *     (RAX, XMM0, etc.) at a RET instruction.
 *   - **Pointer argument writes:** stores through pointer arguments
 *     (arguments whose type was inferred as Pointer by Type Inference).
 *   - **Global memory writes:** stores to non-stack, non-argument memory.
 *   - **I/O / syscall side effects:** CALL instructions to known I/O functions,
 *     or INT / SYSCALL / SYSENTER instructions.
 *   - **Volatile accesses:** stores/loads flagged as volatile (detected from
 *     compiler annotations or MMIO address ranges).
 *
 * Backward propagation: starting from live roots, mark all instructions that
 * define values used (transitively) by live instructions as live.  Anything
 * not reachable from a C-level live root is dead.
 *
 * ### Phase 3 — Unreachable Block Elimination
 *
 * After CFG structuring (Stage 20), some basic blocks may have become
 * unreachable (no path from the entry block).  These are entirely eliminated.
 *
 * ## Output
 *
 * `DeadCodeResult` records:
 *   - `eliminatedInstrs`:    set of `InstrId`s marked dead and removed.
 *   - `eliminatedBlocks`:    set of `BlockId`s unreachable / emptied.
 *   - `abiArtifactsRemoved`: count by kind.
 *   - `liveInstrs`:          set of `InstrId`s confirmed live (for downstream).
 */

#ifndef RETDEC_DCE_H
#define RETDEC_DCE_H

#include <cstdint>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace retdec {
namespace ssa {
class SSAFunction;
using BlockId = uint32_t;
using InstrId = uint32_t;
using VarId   = uint32_t;
using ValueId = uint32_t;
} // namespace ssa
} // namespace retdec

namespace retdec {
namespace call_conv {
struct CallingConvention;
enum class CC : uint8_t;
} // namespace call_conv
} // namespace retdec

namespace retdec {
namespace dce {

using BlockId = ssa::BlockId;
using InstrId = ssa::InstrId;
using VarId   = ssa::VarId;
using ValueId = ssa::ValueId;

// ─── ABI artifact kinds ───────────────────────────────────────────────────────

enum class AbiArtifactKind : uint8_t {
    StackAlign,       ///< AND RSP, -N — stack alignment adjustment
    ShadowSpaceRead,  ///< Win64 shadow-space read never written by callee
    CalleeSavePair,   ///< Balanced save/restore pair for callee-saved register
    RedZoneAccess,    ///< Store to red zone never read after function exit
    PrologueSetup,    ///< Other prologue instructions (SUB RSP, N)
    EpilogueCleanup,  ///< Other epilogue instructions (ADD RSP, N / LEAVE)
};

inline const char* abiArtifactKindName(AbiArtifactKind k) noexcept {
    switch (k) {
    case AbiArtifactKind::StackAlign:      return "StackAlign";
    case AbiArtifactKind::ShadowSpaceRead: return "ShadowSpaceRead";
    case AbiArtifactKind::CalleeSavePair:  return "CalleeSavePair";
    case AbiArtifactKind::RedZoneAccess:   return "RedZoneAccess";
    case AbiArtifactKind::PrologueSetup:   return "PrologueSetup";
    case AbiArtifactKind::EpilogueCleanup: return "EpilogueCleanup";
    }
    return "?";
}

// ─── ABI artifact descriptor ──────────────────────────────────────────────────

struct AbiArtifact {
    InstrId         instrId   = UINT32_MAX; ///< Primary instruction
    InstrId         pairedId  = UINT32_MAX; ///< Partner (for CalleeSavePair)
    AbiArtifactKind kind      = AbiArtifactKind::StackAlign;
    bool            balanced  = true;       ///< For CalleeSavePair: all paths covered
    VarId           savedReg  = UINT32_MAX; ///< Which callee-save register
};

// ─── Live root kind ──────────────────────────────────────────────────────────

enum class LiveRootKind : uint8_t {
    ReturnValue,     ///< Writes to return-value register at RET
    PtrArgWrite,     ///< Store through a pointer argument
    GlobalWrite,     ///< Store to non-stack, non-arg global memory
    IoSideEffect,    ///< CALL to I/O function, SYSCALL, INT, etc.
    VolatileAccess,  ///< Volatile load or store
};

struct LiveRoot {
    InstrId      instrId = UINT32_MAX;
    LiveRootKind kind    = LiveRootKind::ReturnValue;
};

// ─── Dead code result ─────────────────────────────────────────────────────────

struct DeadCodeResult {
    std::unordered_set<InstrId> eliminatedInstrs;
    std::unordered_set<BlockId> eliminatedBlocks;
    std::unordered_set<InstrId> liveInstrs;
    std::vector<AbiArtifact>    abiArtifacts;

    // Counts by kind
    std::unordered_map<AbiArtifactKind, std::size_t> abiArtifactsRemoved;

    // Summary stats
    std::size_t totalInstrs           = 0;
    std::size_t eliminatedInstrCount  = 0;
    std::size_t eliminatedBlockCount  = 0;

    double eliminationRate() const {
        if (totalInstrs == 0) return 0.0;
        return static_cast<double>(eliminatedInstrCount) / totalInstrs;
    }

    std::string summary() const;
};

// ─── ABI artifact marker ─────────────────────────────────────────────────────

/**
 * Scans the function for ABI-level bookkeeping instructions and marks them
 * as artifacts.
 *
 * Algorithms:
 *
 *  Stack alignment:
 *    Scan prologue (entry block, first N instructions) for AND with RSP as
 *    destination and a negative power-of-two immediate.
 *
 *  Prologue / Epilogue:
 *    SUB RSP, N at function entry → PrologueSetup.
 *    ADD RSP, N / LEAVE just before RET → EpilogueCleanup.
 *
 *  Shadow space reads (Win64):
 *    Load from [RSP+8]..[RSP+40] (positive RBP offsets 8..40 in a
 *    standard frame) that is never preceded by a store to the same slot
 *    from this function.
 *
 *  Callee-save pairs:
 *    Phase 1: find all stores in the prologue whose source VarId matches
 *    a callee-save register name (rbx, r12, r13, r14, r15, rbp, rdi, rsi
 *    on Win64; rbx, r12-r15, rbp on SysV).
 *    Phase 2: for each such store, find a corresponding load that:
 *      (a) loads from the same stack slot, and
 *      (b) is in a block that dominates every RET instruction.
 *    Mark as balanced if found; unbalanced otherwise (exception paths).
 *
 *  Red zone:
 *    Stores to [RSP - N] for N ∈ [1,128] whose destination is only read
 *    within the current function (never escapes) and is dead at exit.
 */
class AbiArtifactMarker {
public:
    struct Config {
        bool win64        = false;  ///< Win64 shadow-space detection
        bool sysVAmd64    = true;   ///< SysV red zone detection
        bool arm32        = false;
        bool aarch64      = false;
    };
    static Config defaultConfig() noexcept { return {}; }

    std::vector<AbiArtifact> run(const ssa::SSAFunction& fn,
                                  const Config& cfg = defaultConfig()) const;

private:
    std::vector<AbiArtifact> markStackAlign(const ssa::SSAFunction& fn) const;
    std::vector<AbiArtifact> markPrologueEpilogue(const ssa::SSAFunction& fn) const;
    std::vector<AbiArtifact> markShadowSpace(const ssa::SSAFunction& fn) const;
    std::vector<AbiArtifact> markCalleeSavePairs(const ssa::SSAFunction& fn,
                                                   const Config& cfg) const;
    std::vector<AbiArtifact> markRedZone(const ssa::SSAFunction& fn) const;

    bool isCalleeSaveReg(const std::string& name, bool win64) const;
    bool isRspAlignInstr(const ssa::SSAFunction& fn, InstrId id) const;
    bool isPrologueBlock(BlockId blk, const ssa::SSAFunction& fn) const;
};

// ─── Live root collector ──────────────────────────────────────────────────────

/**
 * Identifies C-semantic live roots — instructions whose effects are
 * observable from outside the function.
 *
 * For each category:
 *
 *   ReturnValue:
 *     Every RET instruction is a live root, plus all instructions that
 *     define values live-out at a RET block (i.e. the return register defs).
 *
 *   PtrArgWrite:
 *     Any Store whose base address comes from (or is derived from) an
 *     SSA value that was identified as a pointer argument.
 *
 *   GlobalWrite:
 *     Any Store to a non-stack MemRef (memIsStack == false).
 *
 *   IoSideEffect:
 *     CALL / SYSCALL / INT instructions.  All CALLs are conservatively
 *     treated as side-effectful (inter-procedural analysis is a separate pass).
 *
 *   VolatileAccess:
 *     Any Load/Store whose address is in a known MMIO range, or flagged
 *     by debug/type information as `volatile`.
 */
class LiveRootCollector {
public:
    std::vector<LiveRoot> run(const ssa::SSAFunction& fn,
                               const call_conv::CallingConvention& cc) const;

private:
    void collectReturnRoots(const ssa::SSAFunction& fn,
                              std::vector<LiveRoot>& roots) const;
    void collectPtrArgWrites(const ssa::SSAFunction& fn,
                               const call_conv::CallingConvention& cc,
                               std::vector<LiveRoot>& roots) const;
    void collectGlobalWrites(const ssa::SSAFunction& fn,
                               std::vector<LiveRoot>& roots) const;
    void collectIoSideEffects(const ssa::SSAFunction& fn,
                                std::vector<LiveRoot>& roots) const;
};

// ─── Dead propagation ────────────────────────────────────────────────────────

/**
 * Performs backward liveness propagation from the collected live roots
 * through the SSA def-use chains.
 *
 * Algorithm:
 *   1. Initialise a worklist with all live root instructions.
 *   2. For each live instruction I:
 *      a. For each use operand V of I:
 *         - Find the defining instruction D of V.
 *         - If D is not yet live, mark it live and add to worklist.
 *      b. For phi operands: mark all contributing definitions live.
 *   3. Any instruction not in the live set after convergence is dead.
 *
 * ABI artifact instructions are never added to the worklist, even if their
 * output is used — their uses are already accounted for by the balanced-pair
 * check in the ABI marker.
 */
class DeadPropagation {
public:
    /// After run(), liveInstrs contains all instructions reachable from roots.
    std::unordered_set<InstrId> run(
        const ssa::SSAFunction& fn,
        const std::vector<LiveRoot>& roots,
        const std::unordered_set<InstrId>& abiArtifactInstrs) const;
};

// ─── Unreachable block eliminator ────────────────────────────────────────────

/**
 * Finds and marks basic blocks that are unreachable from the function entry
 * by a simple forward reachability analysis (BFS/DFS on the CFG).
 *
 * Blocks within irreducible regions that were "structurally isolated" by
 * the CFG structurer are also candidates.
 */
class UnreachableEliminator {
public:
    std::unordered_set<BlockId> run(const ssa::SSAFunction& fn) const;
};

// ─── Main DCE pass ───────────────────────────────────────────────────────────

/**
 * Orchestrates the full ABI-filtered DCE pipeline:
 *   1. AbiArtifactMarker
 *   2. LiveRootCollector
 *   3. DeadPropagation
 *   4. UnreachableEliminator
 *   5. Merge results into DeadCodeResult
 *
 * Note: this pass is *analysis-only* — it does not mutate the SSAFunction.
 * The actual removal of instructions is done by the code generator (Stage 24),
 * which queries `DeadCodeResult::liveInstrs` to decide what to emit.
 */
class DcePass {
public:
    struct Config {
        AbiArtifactMarker::Config abiCfg;
        bool eliminateUnreachableBlocks = true;
        bool keepAbiArtifacts           = false; ///< debug: disable ABI filtering
        bool verboseStats               = false;
    };
    static Config defaultConfig() noexcept { return {}; }

    DeadCodeResult run(const ssa::SSAFunction& fn,
                        const call_conv::CallingConvention& cc,
                        const Config& cfg = defaultConfig()) const;
};

} // namespace dce
} // namespace retdec

#endif // RETDEC_DCE_H
