/**
 * @file include/retdec/cfg/cfg.h
 * @brief Typed-edge Control Flow Graph with jump table and vtable resolution.
 *
 * ## Edge types
 *
 *   FallThrough        — sequential fallthrough (unconditional continuation)
 *   TrueBranch         — conditional branch taken
 *   FalseBranch        — conditional branch not taken
 *   DirectCall         — direct CALL instruction
 *   TailCall           — tail call (JMP to another function)
 *   SwitchEdge         — one arm of a resolved jump table
 *   ExceptionEdge      — SEH/DWARF exception handler
 *   VirtualCallEdge    — resolved virtual dispatch target
 *   LoopBackEdge       — back edge detected during DFS (loop latch → header)
 *   UnresolvedIndirect — indirect branch that could not be resolved
 *
 * ## Construction pipeline
 *
 *   Phase 1 — Direct edges
 *     Walk decoded instructions; emit typed edges for all direct control-flow
 *     transfers (JMP, Jcc, CALL, RET, CALL tailcall pattern).
 *
 *   Phase 2 — Indirect resolution (priority order)
 *     (a) Exception tables  → ExceptionEdge per handler
 *     (b) Jump tables       → SwitchEdge per table entry
 *         Detection: CMP reg, bound; JA/JAE preceding indirect JMP/JMP [table+reg*stride]
 *         Formats: GCC = absolute VAs; MSVC = int32 offsets from table base;
 *                  Clang = GCC-style
 *     (c) Virtual calls     → VirtualCallEdge per vtable slot candidate
 *     (d) Remaining         → UnresolvedIndirectEdge + diagnostic
 *
 *   Phase 3 — Edge typing
 *     DFS colouring (grey/black) to detect back edges.
 *     Back edges inside the same function are reclassified as LoopBackEdge.
 *
 * ## Usage
 *
 *   CFGBuilder builder(imageBase, data, size, is64Bit);
 *   builder.addFunction(0x401000, 0x401100, decodedInstrs);
 *   builder.addJumpTable(0x401050, tableBase, 8, stride4, JumpTableFmt::GCC);
 *   builder.addVtable(0x403000, {0x401100, 0x401200});
 *   builder.addExceptionHandler(0x401010, 0x401300);
 *   builder.build();
 *
 *   const CFGGraph& g = builder.graph();
 *   for (auto& [addr, node] : g.nodes) { ... }
 */

#ifndef RETDEC_CFG_CFG_H
#define RETDEC_CFG_CFG_H

#include <cstdint>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace retdec {
namespace cfg {

// ─── Edge type ────────────────────────────────────────────────────────────────

enum class EdgeType : uint8_t {
    FallThrough,
    TrueBranch,
    FalseBranch,
    DirectCall,
    TailCall,
    SwitchEdge,
    ExceptionEdge,
    VirtualCallEdge,
    LoopBackEdge,
    UnresolvedIndirect,
};

inline const char* edgeTypeName(EdgeType t) noexcept
{
    switch (t) {
    case EdgeType::FallThrough:        return "FallThrough";
    case EdgeType::TrueBranch:         return "TrueBranch";
    case EdgeType::FalseBranch:        return "FalseBranch";
    case EdgeType::DirectCall:         return "DirectCall";
    case EdgeType::TailCall:           return "TailCall";
    case EdgeType::SwitchEdge:         return "SwitchEdge";
    case EdgeType::ExceptionEdge:      return "ExceptionEdge";
    case EdgeType::VirtualCallEdge:    return "VirtualCallEdge";
    case EdgeType::LoopBackEdge:       return "LoopBackEdge";
    case EdgeType::UnresolvedIndirect: return "UnresolvedIndirect";
    }
    return "Unknown";
}

// ─── CFG Edge ─────────────────────────────────────────────────────────────────

struct CFGEdge {
    uint64_t from;          ///< Source basic block start address
    uint64_t to;            ///< Target address (0 if unresolved)
    EdgeType type;
    uint32_t switchIndex = 0; ///< For SwitchEdge: the case index (0-based)

    bool isResolved()  const noexcept { return to != 0; }
    bool isBackEdge()  const noexcept { return type == EdgeType::LoopBackEdge; }
    bool isCallEdge()  const noexcept {
        return type == EdgeType::DirectCall   ||
               type == EdgeType::TailCall     ||
               type == EdgeType::VirtualCallEdge;
    }
};

// ─── Basic block ──────────────────────────────────────────────────────────────

struct BasicBlock {
    uint64_t startAddr   = 0; ///< First instruction VMA
    uint64_t endAddr     = 0; ///< One past last byte of last instruction
    uint64_t functionAddr= 0; ///< Owning function start address

    // Successor edges leaving this block.
    std::vector<CFGEdge> succs;
    // Predecessor addresses (for quick reverse lookup).
    std::vector<uint64_t> preds;

    bool isEntry()    const noexcept { return startAddr == functionAddr; }
    bool hasReturn()  const noexcept {
        for (const auto& e : succs) {
            if (e.type == EdgeType::FallThrough && e.to == 0) return true;
        }
        return false;
    }
};

// ─── Jump table format ────────────────────────────────────────────────────────

enum class JumpTableFmt : uint8_t {
    GCC,   ///< Absolute virtual addresses
    MSVC,  ///< int32 offsets from table base address
    Clang, ///< Same as GCC
};

// ─── Jump table descriptor ────────────────────────────────────────────────────

struct JumpTableInfo {
    uint64_t     instrAddr;  ///< Address of the indirect JMP instruction
    uint64_t     tableBase;  ///< VA of the jump table array
    uint32_t     numEntries; ///< Number of switch cases
    uint32_t     stride;     ///< Bytes per entry (4 or 8)
    JumpTableFmt fmt;        ///< Table format
};

// ─── Vtable descriptor ────────────────────────────────────────────────────────

struct VtableInfo {
    uint64_t             tableAddr;   ///< VA of the vtable
    std::vector<uint64_t> slots;      ///< Function pointer for each slot
};

// ─── CFG Graph ────────────────────────────────────────────────────────────────

struct CFGGraph {
    /// Map from basic-block start address to BasicBlock.
    std::unordered_map<uint64_t, BasicBlock> nodes;

    /// Unresolved indirect branches with a diagnostic message.
    struct Diagnostic {
        uint64_t    addr;
        std::string reason;
    };
    std::vector<Diagnostic> diagnostics;

    // ── Query helpers ─────────────────────────────────────────────────────────

    /// Return the BasicBlock containing [addr], or nullptr.
    const BasicBlock* blockAt(uint64_t addr) const noexcept;

    /// All successors of the block starting at [addr].
    std::vector<CFGEdge> successorsOf(uint64_t blockAddr) const;

    /// All predecessor block start addresses of [addr].
    std::vector<uint64_t> predecessorsOf(uint64_t blockAddr) const;

    /// Count edges of a given type across the entire graph.
    std::size_t countEdges(EdgeType t) const noexcept;

    /// Total edge count.
    std::size_t totalEdges() const noexcept;
};

// ─── Instruction summary (input to CFGBuilder) ────────────────────────────────
//
// CFGBuilder does not depend on the full SemDecoder output — it only needs the
// control-flow summary of each instruction.

enum class InstrKind : uint8_t {
    Normal,           ///< Not a control-flow transfer
    DirectJmp,        ///< Unconditional direct JMP rel
    ConditionalJmp,   ///< Conditional Jcc rel
    IndirectJmp,      ///< JMP [mem] or JMP reg
    DirectCall,       ///< CALL rel
    IndirectCall,     ///< CALL [mem] or CALL reg
    Ret,              ///< RET / RETF
    TailCall,         ///< JMP to a different function (tail call)
};

struct InstrSummary {
    uint64_t  addr       = 0;
    uint32_t  len        = 0;
    InstrKind kind       = InstrKind::Normal;
    uint64_t  target     = 0;   ///< Direct branch/call target (0 if indirect/unknown)
    bool      isConditional = false;
};

// ─── CFGBuilder ───────────────────────────────────────────────────────────────

class CFGBuilder {
public:
    /**
     * @param imageBase  Virtual base address.
     * @param data       Raw image bytes (caller keeps alive).
     * @param size       Image size in bytes.
     * @param is64Bit    True for 64-bit images.
     */
    CFGBuilder(uint64_t       imageBase,
               const uint8_t* data,
               std::size_t    size,
               bool           is64Bit = true);

    ~CFGBuilder() = default;

    // ── Input: function instruction sequences ─────────────────────────────────

    /**
     * Register the instruction sequence for one function.
     * @param funcStart  VMA of function entry.
     * @param funcEnd    One past last byte of function.
     * @param instrs     Linear instruction sequence (from SemDecoder).
     */
    void addFunction(uint64_t                         funcStart,
                     uint64_t                         funcEnd,
                     const std::vector<InstrSummary>& instrs);

    // ── Input: indirect resolution hints ─────────────────────────────────────

    void addJumpTable(const JumpTableInfo& jt);
    void addVtable(const VtableInfo& vt);
    void addExceptionHandler(uint64_t instrAddr, uint64_t handlerAddr);

    // ── Build ─────────────────────────────────────────────────────────────────

    /**
     * Run all three phases and populate the internal CFGGraph.
     */
    void build();

    /// Access the completed graph.
    const CFGGraph& graph() const noexcept { return _graph; }
    CFGGraph&       graph()       noexcept { return _graph; }

    // ── Phase steps (exposed for testing) ────────────────────────────────────

    void runPhase1();
    void runPhase2();
    void runPhase3();

    // ── Jump table resolution (exposed for testing) ───────────────────────────

    /**
     * Resolve one jump table and return the list of target VAs.
     * Returns empty if tableBase or entries are out-of-image.
     */
    std::vector<uint64_t> resolveJumpTable(const JumpTableInfo& jt) const;

    // ── Jump table detection (VSA-lite bounds-check heuristic) ────────────────

    /**
     * Scan the instruction window preceding `indirectJmpAddr` for a
     * CMP reg, imm + JA/JAE guard pattern.  Returns the bound (numEntries)
     * if found, or 0 otherwise.
     */
    uint32_t detectJumpTableBound(uint64_t indirectJmpAddr) const noexcept;

    /**
     * Scan for the jump table base address (the constant loaded into the
     * index register before the indirect JMP).  Returns the table VA or 0.
     */
    uint64_t detectJumpTableBase(uint64_t indirectJmpAddr) const noexcept;

private:
    uint64_t       _imageBase;
    const uint8_t* _data;
    std::size_t    _size;
    bool           _is64Bit;

    // ── Input data ─────────────────────────────────────────────────────────────

    struct FunctionInfo {
        uint64_t                  start;
        uint64_t                  end;
        std::vector<InstrSummary> instrs;
    };
    std::vector<FunctionInfo>  _functions;
    std::vector<JumpTableInfo> _jumpTables;
    std::vector<VtableInfo>    _vtables;

    // Exception handler map: indirect JMP addr → handler addr.
    std::unordered_map<uint64_t, uint64_t> _exHandlers;

    // Set of known function entry points (for tail-call classification).
    std::unordered_set<uint64_t> _funcEntries;

    CFGGraph _graph;

    // ── Phase 1 helpers ────────────────────────────────────────────────────────

    void buildBlocksForFunction(const FunctionInfo& fi);

    // ── Phase 2 helpers ────────────────────────────────────────────────────────

    void resolveExceptionEdges();
    void resolveJumpTables();
    void resolveVirtualCalls();
    void emitUnresolvedDiagnostics();

    // ── Phase 3 helpers ────────────────────────────────────────────────────────

    void classifyBackEdges();
    // DFS that tags back edges.
    void dfsVisit(uint64_t blockAddr,
                  std::unordered_map<uint64_t, int>& colour);

    // ── Raw memory helpers ─────────────────────────────────────────────────────

    std::size_t vaToOffset(uint64_t va) const noexcept;
    bool        inBounds(uint64_t va, std::size_t sz) const noexcept;
    int32_t     readI32(uint64_t va) const noexcept;
    uint32_t    readU32(uint64_t va) const noexcept;
    uint64_t    readU64(uint64_t va) const noexcept;
    uint64_t    readPtr(uint64_t va) const noexcept; ///< readU32 or readU64

    // ── Block helpers ─────────────────────────────────────────────────────────

    BasicBlock& ensureBlock(uint64_t addr, uint64_t funcStart);
    void        addEdge(uint64_t from, uint64_t to, EdgeType type,
                        uint32_t switchIdx = 0);
    void        splitBlockAt(uint64_t splitAddr);
};

} // namespace cfg
} // namespace retdec

#endif // RETDEC_CFG_CFG_H
