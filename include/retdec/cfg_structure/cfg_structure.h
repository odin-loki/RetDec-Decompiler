/**
 * @file include/retdec/cfg_structure/cfg_structure.h
 * @brief Compiler-Aware SESE CFG Structuring with Incremental Dominators.
 *
 * ## Overview
 *
 * This module transforms a raw Control-Flow Graph (CFG) — consisting of
 * basic blocks and edges — into a structured representation suitable for
 * high-quality C code generation.  The output is a tree of `StructNode`
 * objects: each node represents an if/else, loop (while/for/do-while),
 * switch, or sequence.
 *
 * The structuring algorithm has five stages:
 *
 * ### Stage 1 — Irreducibility Detection
 *
 * A CFG is reducible iff every back-edge (an edge whose target dominates
 * its source) goes to a natural loop header.  An irreducible CFG has at
 * least one back-edge whose target does NOT dominate the source (i.e. it
 * is a back-edge to a non-ancestor in the DFS tree).
 *
 * We detect irreducibility in a single DFS pass:
 *   - Classify every edge as tree, forward, cross, or back.
 *   - A cross edge u→v where v dominates u is a natural loop back-edge.
 *   - A cross edge u→v where v does NOT dominate u marks an irreducible
 *     region.
 *
 * For irreducible regions: we emit the blocks verbatim and insert `goto`
 * statements.  We add a comment explaining that this region is irreducible
 * (rare in practice; < 1% of compiler-generated code).
 *
 * ### Stage 2 — Post-Dominator Tree
 *
 * The post-dominator tree is computed over the reversed CFG using the same
 * Lengauer-Tarjan algorithm as the forward dominator tree.  In the reversed
 * CFG, we add a virtual exit node with edges from all return nodes.
 *
 * Post-dominator information is needed for:
 *   - Identifying the reconvergence point of an if-then-else (= immediate
 *     post-dominator of the branch).
 *   - Identifying the extent of loop body (= latch post-dominates header).
 *   - Switch statement reconvergence.
 *
 * ### Stage 3 — SESE Region Decomposition
 *
 * A Single-Entry Single-Exit (SESE) region is a set of nodes with exactly
 * one entry edge and one exit edge in the CFG.
 *
 * We use DFS timestamps (entry_time / exit_time) to define regions:
 *   - Node v is inside region [entry, exit] iff
 *     entry_time[entry] ≤ entry_time[v] ≤ exit_time[v] ≤ exit_time[exit].
 *   - Containment check is O(1).
 *
 * We identify SESE regions by finding pairs (h, e) where:
 *   - h is a branch or loop header.
 *   - e is h's immediate post-dominator.
 *   - All paths from h to e stay within the CFG region
 *     [entry_time[h], exit_time[h]].
 *
 * ### Stage 4 — Loop Type Recovery
 *
 * For each natural loop (identified by back-edges to dominating headers):
 *
 *   `while` loop:
 *     - Condition at the loop header (conditional branch).
 *     - One exit edge leaves the loop, one stays inside.
 *     - Back-edge from latch → header.
 *
 *   `do-while` loop:
 *     - Condition at the latch block (last block before back-edge).
 *     - Entry to loop body is unconditional.
 *
 *   `for` loop:
 *     - Like `while` but with an identifiable increment assignment at the
 *       latch (ADD / INC to a loop-varying variable).
 *     - We prefer `for` over `while` when the increment is detected.
 *
 *   Infinite loop:
 *     - No exit edge from the loop body.
 *     - `for(;;)` or `while(true)`.
 *
 * ### Stage 5 — Compiler-Specific Pattern Matching
 *
 * Different compilers generate slightly different CFG shapes for the same
 * source construct.  We try compiler-specific patterns first:
 *
 *   GCC/Clang if-then-else:
 *     Header → then_block → merge, Header → else_block → merge.
 *     Merge = ipdom(Header).
 *
 *   MSVC if-then (no else):
 *     Header → then_block → merge, Header → merge (fall-through).
 *
 *   Short-circuit evaluation:
 *     GCC: &&/|| expands to a chain of conditional branches.
 *     MSVC: same.  We recognise the pattern when consecutive branches
 *     share the same "false" target (&&) or "true" target (||).
 *
 *   Switch:
 *     A single block with fan-out degree ≥ 3 reconverging at ipdom.
 *     Case targets are sorted by their case value if available.
 *
 * ## Output: StructNode tree
 *
 * The structured output is a tree of `StructNode` objects:
 *
 *   StructNode::Sequence   — linear list of nodes
 *   StructNode::IfThen     — if (cond) { then }
 *   StructNode::IfThenElse — if (cond) { then } else { else }
 *   StructNode::While      — while (cond) { body }
 *   StructNode::DoWhile    — do { body } while (cond)
 *   StructNode::For        — for (init; cond; incr) { body }
 *   StructNode::Switch     — switch (expr) { cases }
 *   StructNode::Goto       — goto label (irreducible / break-out)
 *   StructNode::Block      — a single basic block (leaf)
 *   StructNode::Infinite   — for(;;) / while(true)
 */

#ifndef RETDEC_CFG_STRUCTURE_H
#define RETDEC_CFG_STRUCTURE_H

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace retdec {
namespace ssa {
class SSAFunction;
} // namespace ssa
} // namespace retdec

namespace retdec {
namespace cfg_structure {

using BlockId = uint32_t;
static constexpr BlockId kInvalidBlock = UINT32_MAX;

// ─── Edge classification ──────────────────────────────────────────────────────

enum class EdgeKind : uint8_t {
    Tree,       ///< DFS tree edge
    Forward,    ///< forward edge (ancestor → non-child descendant)
    Cross,      ///< cross edge (non-ancestor/descendant)
    Back,       ///< back edge (descendant → ancestor, natural loop)
    IrreducibleBack, ///< back edge to a non-dominating node
};

struct CfgEdge {
    BlockId  src;
    BlockId  dst;
    EdgeKind kind;
};

// ─── Loop kind ────────────────────────────────────────────────────────────────

enum class LoopKind : uint8_t {
    Unknown,
    While,      ///< condition at header, test-first
    DoWhile,    ///< condition at latch, test-last
    For,        ///< while + identifiable increment at latch
    Infinite,   ///< no exit edge (for(;;))
};

inline const char* loopKindName(LoopKind k) noexcept {
    switch (k) {
    case LoopKind::While:    return "while";
    case LoopKind::DoWhile:  return "do-while";
    case LoopKind::For:      return "for";
    case LoopKind::Infinite: return "infinite";
    default:                 return "unknown";
    }
}

// ─── Natural loop ─────────────────────────────────────────────────────────────

struct NaturalLoop {
    BlockId  header  = kInvalidBlock;
    BlockId  latch   = kInvalidBlock;  ///< block containing the back-edge
    LoopKind kind    = LoopKind::Unknown;
    std::vector<BlockId> body;          ///< all blocks in the loop
    std::vector<BlockId> exits;         ///< blocks with edges leaving the loop
    bool     hasIncrement = false;      ///< latch has an identifiable increment
    uint32_t inductionVar = UINT32_MAX; ///< SSA value ID of the induction variable
};

// ─── SESE region ─────────────────────────────────────────────────────────────

struct SESERegion {
    BlockId  entry     = kInvalidBlock;
    BlockId  exit_node = kInvalidBlock;
    uint32_t entryTime = 0;   ///< DFS discovery time of entry
    uint32_t exitTime  = 0;   ///< DFS finish time of entry
    std::vector<BlockId> nodes;

    bool contains(uint32_t nodeEntry, uint32_t nodeExit) const {
        return entryTime <= nodeEntry && nodeExit <= exitTime;
    }
};

// ─── Structured output node ───────────────────────────────────────────────────

struct StructNode {
    enum class Kind : uint8_t {
        Block,
        Sequence,
        IfThen,
        IfThenElse,
        While,
        DoWhile,
        For,
        Switch,
        Goto,
        Infinite,
    };

    Kind    kind = Kind::Block;
    BlockId blockId = kInvalidBlock;   ///< for Kind::Block

    // Condition value (for If/While/DoWhile/For)
    uint32_t condValueId = UINT32_MAX;

    // Loop fields
    LoopKind loopKind = LoopKind::Unknown;
    uint32_t inductionVar = UINT32_MAX;  ///< for For loops
    uint32_t incrementInstrId = UINT32_MAX;

    // Switch fields
    std::vector<std::pair<int64_t, std::unique_ptr<StructNode>>> cases;
    std::unique_ptr<StructNode> defaultCase;

    // Goto target
    BlockId  gotoTarget = kInvalidBlock;
    std::string label;
    bool     isIrreducible = false;  ///< true if goto due to irreducibility

    // Children
    std::vector<std::unique_ptr<StructNode>> children;

    // Factory helpers
    static std::unique_ptr<StructNode> block(BlockId id) {
        auto n = std::make_unique<StructNode>();
        n->kind = Kind::Block; n->blockId = id; return n;
    }
    static std::unique_ptr<StructNode> seq() {
        auto n = std::make_unique<StructNode>();
        n->kind = Kind::Sequence; return n;
    }
    static std::unique_ptr<StructNode> ifThen(uint32_t cond) {
        auto n = std::make_unique<StructNode>();
        n->kind = Kind::IfThen; n->condValueId = cond; return n;
    }
    static std::unique_ptr<StructNode> ifThenElse(uint32_t cond) {
        auto n = std::make_unique<StructNode>();
        n->kind = Kind::IfThenElse; n->condValueId = cond; return n;
    }
    static std::unique_ptr<StructNode> whileLoop(uint32_t cond) {
        auto n = std::make_unique<StructNode>();
        n->kind = Kind::While; n->condValueId = cond;
        n->loopKind = LoopKind::While; return n;
    }
    static std::unique_ptr<StructNode> doWhileLoop(uint32_t cond) {
        auto n = std::make_unique<StructNode>();
        n->kind = Kind::DoWhile; n->condValueId = cond;
        n->loopKind = LoopKind::DoWhile; return n;
    }
    static std::unique_ptr<StructNode> forLoop(uint32_t cond, uint32_t induction) {
        auto n = std::make_unique<StructNode>();
        n->kind = Kind::For; n->condValueId = cond;
        n->loopKind = LoopKind::For; n->inductionVar = induction; return n;
    }
    static std::unique_ptr<StructNode> infinite() {
        auto n = std::make_unique<StructNode>();
        n->kind = Kind::Infinite; n->loopKind = LoopKind::Infinite; return n;
    }
    static std::unique_ptr<StructNode> gotoNode(BlockId target, bool irred = false) {
        auto n = std::make_unique<StructNode>();
        n->kind = Kind::Goto; n->gotoTarget = target;
        n->isIrreducible = irred; return n;
    }

    const char* kindName() const noexcept {
        switch (kind) {
        case Kind::Block:      return "Block";
        case Kind::Sequence:   return "Sequence";
        case Kind::IfThen:     return "IfThen";
        case Kind::IfThenElse: return "IfThenElse";
        case Kind::While:      return "While";
        case Kind::DoWhile:    return "DoWhile";
        case Kind::For:        return "For";
        case Kind::Switch:     return "Switch";
        case Kind::Goto:       return "Goto";
        case Kind::Infinite:   return "Infinite";
        }
        return "?";
    }
};

// ─── Irreducibility check ─────────────────────────────────────────────────────

/**
 * Determines whether a CFG is reducible by detecting back-edges to
 * non-dominating nodes during a single DFS traversal.
 */
class IrreducibilityCheck {
public:
    struct Result {
        bool isReducible = true;
        std::vector<CfgEdge> backEdges;
        std::vector<CfgEdge> irreducibleEdges;
        std::vector<CfgEdge> allEdges;
    };

    Result run(const ssa::SSAFunction& fn) const;

private:
    void dfs(BlockId v,
             const ssa::SSAFunction& fn,
             std::vector<uint8_t>& color,        // 0=white,1=grey,2=black
             std::vector<BlockId>& parent,
             std::vector<uint32_t>& disc,
             const std::vector<BlockId>& idom,
             Result& res,
             uint32_t& timer) const;

    bool dominates(const std::vector<BlockId>& idom,
                   const std::vector<uint32_t>& disc,
                   BlockId a, BlockId b) const;
};

// ─── Post-dominator tree ──────────────────────────────────────────────────────

/**
 * Computes the post-dominator tree using Lengauer-Tarjan on the reversed CFG.
 * A virtual exit node (blockCount) is added with edges from all terminal blocks.
 */
class PostDomTree {
public:
    void run(const ssa::SSAFunction& fn);

    /// Immediate post-dominator of block b (-1 for exit / none).
    BlockId ipostdom(BlockId b) const;

    /// True if a post-dominates b (every path from b to exit passes through a).
    bool postDominates(BlockId a, BlockId b) const;

    std::size_t nodeCount() const { return ipdom_.size(); }

private:
    std::vector<BlockId>  ipdom_;
    std::vector<uint32_t> rpo_;
    std::vector<BlockId>  vertex_;

    uint32_t find(uint32_t x) const;
    void compress(uint32_t v);

    mutable std::vector<uint32_t> semi_;
    mutable std::vector<uint32_t> ancestor_;
    mutable std::vector<uint32_t> label_;
    mutable std::vector<uint32_t> parent_;
    std::vector<std::vector<uint32_t>> bucket_;
};

// ─── SESE decomposer ──────────────────────────────────────────────────────────

/**
 * Identifies Single-Entry Single-Exit (SESE) regions from DFS timestamps
 * and the post-dominator tree.
 *
 * Each region is defined by (entry_block, exit_block) where exit_block is
 * the immediate post-dominator of entry_block.
 */
class SESEDecomposer {
public:
    void run(const ssa::SSAFunction& fn, const PostDomTree& pdom);

    const std::vector<SESERegion>& regions() const { return regions_; }

    /// Check if node v (by DFS times) is inside region r.
    bool contains(const SESERegion& r, BlockId v) const;

    uint32_t discTime(BlockId b) const;
    uint32_t finTime(BlockId b)  const;

private:
    std::vector<SESERegion> regions_;
    std::unordered_map<BlockId, uint32_t> disc_;
    std::unordered_map<BlockId, uint32_t> fin_;

    void dfsTimestamp(BlockId v, const ssa::SSAFunction& fn,
                       std::unordered_set<BlockId>& visited, uint32_t& timer);
};

// ─── Loop recovery ────────────────────────────────────────────────────────────

/**
 * Classifies each natural loop (identified by back-edges) as while/for/do-while.
 *
 * Classification rules:
 *   For loop:    header has conditional branch AND latch has an ADD/INC
 *                to an SSA value that is also used in the header condition.
 *   While loop:  header has conditional branch, no identifiable increment.
 *   Do-while:    header is unconditional; latch has conditional branch.
 *   Infinite:    no exit edge from any loop body block.
 */
class LoopRecovery {
public:
    std::vector<NaturalLoop> run(const ssa::SSAFunction& fn,
                                  const IrreducibilityCheck::Result& edges,
                                  const std::vector<BlockId>& idom) const;

private:
    std::vector<BlockId> collectLoopBody(BlockId header, BlockId latch,
                                          const ssa::SSAFunction& fn) const;

    LoopKind classifyLoop(const ssa::SSAFunction& fn,
                           NaturalLoop& loop) const;

    bool hasIncrementAtLatch(const ssa::SSAFunction& fn,
                               BlockId latch,
                               uint32_t& outInductionVar) const;

    bool hasConditionalBranch(const ssa::SSAFunction& fn,
                               BlockId block) const;
};

// ─── Compiler structurer ──────────────────────────────────────────────────────

/**
 * Main structuring pass.  Produces a `StructNode` tree from the CFG.
 *
 * Algorithm (recursive descent over SESE regions):
 *   1. Process the entry block.
 *   2. If it ends in an unconditional branch → sequence, recurse on successor.
 *   3. If it ends in a conditional branch:
 *      a. Find the reconvergence point = ipdom(entry).
 *      b. If one successor == ipdom → IfThen(then branch).
 *      c. Else → IfThenElse(then, else branches).
 *   4. If the block is a loop header → emit the appropriate loop kind,
 *      recurse on the loop body.
 *   5. If the block has ≥ 3 successors → Switch.
 *   6. If the block is the exit of the current SESE region → stop.
 *   7. Irreducible regions → emit Goto nodes.
 */
class CompilerStructurer {
public:
    struct Config {
        bool preferFor  = true;   ///< promote While to For when possible
        bool emitGotoComment = true;
    };
    static Config defaultConfig() noexcept { return {}; }

    std::unique_ptr<StructNode> run(const ssa::SSAFunction& fn,
                                     const Config& cfg = defaultConfig()) const;

private:
    std::unique_ptr<StructNode> structure(
        BlockId entry,
        BlockId exitNode,
        const ssa::SSAFunction& fn,
        const PostDomTree& pdom,
        const SESEDecomposer& sese,
        const std::vector<NaturalLoop>& loops,
        const IrreducibilityCheck::Result& irred,
        std::unordered_set<BlockId>& visited,
        const Config& cfg) const;

    const NaturalLoop* findLoop(BlockId header,
                                 const std::vector<NaturalLoop>& loops) const;

    bool isLoopHeader(BlockId b,
                       const std::vector<NaturalLoop>& loops) const;

    bool isIrreducibleEntry(BlockId b,
                              const IrreducibilityCheck::Result& irred) const;

    uint32_t condValueOf(BlockId b, const ssa::SSAFunction& fn) const;
};

// ─── Main structuring pass ────────────────────────────────────────────────────

/**
 * Orchestrates the full structuring pipeline:
 *   1. IrreducibilityCheck
 *   2. PostDomTree
 *   3. SESEDecomposer
 *   4. LoopRecovery
 *   5. CompilerStructurer
 */
class CfgStructurePass {
public:
    struct Stats {
        bool isReducible    = true;
        std::size_t loopsFound  = 0;
        std::size_t whileLoops  = 0;
        std::size_t doWhileLoops= 0;
        std::size_t forLoops    = 0;
        std::size_t infiniteLoops= 0;
        std::size_t ifThenNodes = 0;
        std::size_t ifThenElseNodes= 0;
        std::size_t switchNodes = 0;
        std::size_t gotoNodes   = 0;
        std::size_t seseRegions = 0;
    };

    std::unique_ptr<StructNode> run(const ssa::SSAFunction& fn,
                                     const CompilerStructurer::Config& cfg = CompilerStructurer::Config{});

    const Stats& stats() const { return stats_; }
    const IrreducibilityCheck::Result& irreducibility() const { return irred_; }
    const std::vector<NaturalLoop>& loops() const { return loops_; }

private:
    Stats stats_;
    IrreducibilityCheck::Result irred_;
    std::vector<NaturalLoop> loops_;

    void countNodes(const StructNode* node, Stats& s) const;
};

} // namespace cfg_structure
} // namespace retdec

#endif // RETDEC_CFG_STRUCTURE_H
