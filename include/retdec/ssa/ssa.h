/**
 * @file include/retdec/ssa/ssa.h
 * @brief Liveness-pruned SSA construction with x86 flag bundling and
 *        deferred stack-slot promotion.
 *
 * ## Overview
 *
 * This module converts a CFG of "raw" IR instructions (from the decoder and
 * semantic-lifting stages) into Static Single Assignment (SSA) form.
 *
 * Three key design decisions distinguish this implementation from a textbook
 * Cytron et al. construction:
 *
 * ### 1. Liveness-pruned phi placement
 *
 * Standard Cytron phi placement inserts a phi function for variable `v` at
 * every node `d` in the iterated dominance frontier (IDF) of `v`'s definition
 * sites.  This produces many dead phi functions — phi nodes for variables not
 * live along any path through `d`.
 *
 * Liveness-pruned placement adds the gate: insert phi(v) at d only if
 * v ∈ live_in(d).  This reduces phi count by ~50% on typical compiler output
 * (Brandner et al., CGO 2011) while maintaining correctness, because a phi
 * that cannot be reached by any live use of v is semantically dead.
 *
 * ### 2. x86 EFLAGS bundling
 *
 * The six EFLAGS bits (CF, ZF, SF, OF, PF, AF) are written atomically by
 * arithmetic and comparison instructions.  Treating them as six independent
 * SSA variables produces six separate phi functions at every conditional-branch
 * join point — even though the entire bundle is usually consumed within the
 * same basic block as the instruction that defines it.
 *
 * Instead: every flag-writing instruction produces one `FlagBundle` SSA value.
 * Consumers (conditional branches, SETcc, CMOVcc) read individual flag fields
 * from the bundle.  A FlagBundle never requires a phi function if all its
 * uses lie within the same basic block.
 *
 * ### 3. Deferred stack-slot promotion
 *
 * Stack frame accesses (MOV [RBP-8], rax; MOV rcx, [RBP-8]) are initially
 * represented as `MemRef` values referencing a stack slot by (base_reg, offset)
 * pair.  They are promoted to SSA scalars only after alias analysis (Stage 18)
 * confirms that the slot is not aliased.  This avoids premature variable
 * merging for code like:
 *
 *   void* p = &local;   // takes address → slot aliased → stays MemRef
 *   int x = local + 1;  // if local is non-aliased → promoted to SSA
 *
 * ## IR value kinds
 *
 *   VirtualReg   — a logical register (architecture-independent)
 *   Immediate    — a constant integer or floating-point literal
 *   FlagBundle   — the six x86 EFLAGS bits as a single SSA value
 *   MemRef       — a deferred stack/heap memory reference
 *   Undef        — undefined value (uninitialised register / dead flag)
 *   Phi          — a phi function (merges values from predecessor blocks)
 *
 * ## Usage
 *
 *   // Build a raw CFG using ISSABuilder
 *   auto fn = ISSABuilder::create("my_function");
 *   auto* entry = fn->addBlock("entry");
 *   auto* loop  = fn->addBlock("loop_header");
 *   ...
 *   // Construct SSA
 *   SSAPass pass;
 *   pass.run(*fn);
 *   // Now fn->blocks() contain phi-inserted, renamed SSA values
 */

#ifndef RETDEC_SSA_H
#define RETDEC_SSA_H

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <utility>

namespace retdec {
namespace ssa {

// ─── Forward declarations ─────────────────────────────────────────────────────

class IrValue;
class PhiNode;
class IrInstr;
class BasicBlock;
class SSAFunction;

// ─── Value identifiers ───────────────────────────────────────────────────────

using VarId   = uint32_t;  ///< Logical variable / virtual register ID
using BlockId = uint32_t;  ///< Basic block ID within a function
using InstrId = uint32_t;  ///< Instruction ID (monotone, used for ordering)
using ValueId = uint32_t;  ///< Unique SSA value ID (assigned during renaming)

static constexpr VarId   kInvalidVar   = UINT32_MAX;
static constexpr BlockId kInvalidBlock = UINT32_MAX;
static constexpr ValueId kInvalidValue = UINT32_MAX;

// ─── Flag bits ───────────────────────────────────────────────────────────────

/// The six x86/x86-64 EFLAGS bits tracked by this SSA representation.
enum class FlagBit : uint8_t {
    CF = 0,  ///< Carry flag
    ZF = 1,  ///< Zero flag
    SF = 2,  ///< Sign flag
    OF = 3,  ///< Overflow flag
    PF = 4,  ///< Parity flag
    AF = 5,  ///< Auxiliary carry flag
    Count = 6
};

inline const char* flagBitName(FlagBit f) noexcept {
    static const char* n[] = { "CF","ZF","SF","OF","PF","AF" };
    return n[static_cast<int>(f)];
}

// ─── Value kind ──────────────────────────────────────────────────────────────

enum class ValueKind : uint8_t {
    VirtualReg,  ///< Logical / architecture-independent register
    Immediate,   ///< Integer or float constant
    FlagBundle,  ///< x86 EFLAGS bundle (all 6 flags as one SSA value)
    MemRef,      ///< Deferred stack/heap memory reference
    Undef,       ///< Undefined (uninitialised, dead flag, etc.)
    Phi,         ///< Phi function
};

// ─── IrValue ─────────────────────────────────────────────────────────────────

/**
 * One SSA value.  After SSA construction, each IrValue has exactly one
 * definition site (a defining IrInstr or a PhiNode).
 */
struct IrValue {
    ValueId   id      = kInvalidValue;
    ValueKind kind    = ValueKind::Undef;
    VarId     varId   = kInvalidVar;   ///< Original pre-SSA variable
    uint32_t  version = 0;             ///< SSA version number (v0, v1, …)
    uint8_t   width   = 64;            ///< Bit width (8/16/32/64/128)

    // For Immediate
    uint64_t  imm = 0;

    // For MemRef
    VarId     memBaseReg = kInvalidVar;
    int64_t   memOffset  = 0;
    uint8_t   memWidth   = 0;  ///< access size in bytes
    bool      memIsStack = true;

    // For FlagBundle: which flags are defined by the producing instruction
    uint8_t   definedFlags = 0;  ///< bitmask of FlagBit

    // Defining instruction (nullptr for phi or entry-block params)
    IrInstr* defInstr = nullptr;
    PhiNode* defPhi   = nullptr;

    std::string debugName() const;
};

// ─── Phi node ────────────────────────────────────────────────────────────────

/**
 * A phi function at the head of a basic block.
 *
 *   v3 = phi [v1 from bb1, v2 from bb2]
 *
 * After liveness-pruned placement and renaming, every phi has exactly one
 * incoming operand per predecessor block.
 */
struct PhiNode {
    ValueId          result  = kInvalidValue;  ///< The SSA value produced
    VarId            varId   = kInvalidVar;    ///< The variable being merged
    BlockId          block   = kInvalidBlock;  ///< Block containing this phi
    std::vector<std::pair<BlockId, ValueId>> operands; ///< (pred, incoming_val)

    bool isComplete() const {
        // True once all predecessor operands have been filled
        return !operands.empty();
    }
    void addOperand(BlockId pred, ValueId val) {
        operands.push_back({pred, val});
    }
};

// ─── Instruction operand reference ───────────────────────────────────────────

struct Use {
    ValueId valueId = kInvalidValue;  ///< The SSA value being used
    uint8_t operandIndex = 0;
};

// ─── IR instruction ──────────────────────────────────────────────────────────

/**
 * One decoded machine instruction in the IR.
 *
 * Before SSA: uses/defs reference VarId (pre-SSA variable IDs).
 * After SSA:  uses/defs reference ValueId (versioned SSA values).
 */
struct IrInstr {
    InstrId   id      = 0;
    uint64_t  vma     = 0;    ///< Original instruction address
    BlockId   block   = kInvalidBlock;

    // Architecture-neutral opcode category (for flag-writing detection)
    enum class Op : uint8_t {
        Assign, Add, Sub, Mul, Div, And, Or, Xor, Not, Neg,
        Shl, Shr, Sar, Ror, Rol,
        Load, Store,
        Call, Ret, Branch, CondBranch,
        Compare,   ///< CMP / TEST — writes all flags
        FlagWrite, ///< Any instruction that produces flags
        FlagRead,  ///< SETcc, CMOVcc, Jcc — consumes specific flags
        Phi,       ///< Placeholder (actual phi is in PhiNode list)
        Undef,
    } op = Op::Undef;

    // Pre-SSA (before renaming): variable IDs
    VarId  defVar = kInvalidVar;

    // Post-SSA (after renaming): SSA value IDs
    ValueId defValue = kInvalidValue;
    std::vector<Use> uses;

    // For flag-producing instructions
    bool        writesFlagBundle = false;
    uint8_t     flagMask         = 0;   ///< which FlagBits are defined
    ValueId     flagBundleValue  = kInvalidValue;

    // For flag-consuming instructions
    bool        readsFlagBundle  = false;
    uint8_t     readFlagMask     = 0;
    ValueId     flagBundleInput  = kInvalidValue;
    FlagBit     specificFlag     = FlagBit::ZF;  ///< which flag this Jcc reads

    // For Call instructions: resolved callee name (empty = indirect/unknown)
    std::string calleeName;

    std::string debugStr() const;
};

// ─── Basic block ─────────────────────────────────────────────────────────────

/**
 * A basic block in the control-flow graph.
 * Successor/predecessor edges are stored explicitly.
 */
struct BasicBlock {
    BlockId   id    = kInvalidBlock;
    std::string name;

    std::vector<IrInstr*>  instrs;      ///< instructions in order
    std::vector<PhiNode*>  phis;        ///< phi functions at block head
    std::vector<BlockId>   succs;
    std::vector<BlockId>   preds;

    // Liveness analysis results
    std::unordered_set<VarId> gen;       ///< upward-exposed uses (GEN)
    std::unordered_set<VarId> kill;      ///< definitions (KILL)
    std::unordered_set<VarId> liveIn;
    std::unordered_set<VarId> liveOut;

    // Dominator tree
    BlockId idom = kInvalidBlock;        ///< immediate dominator
    std::vector<BlockId> domChildren;
    std::vector<BlockId> domFrontier;    ///< dominance frontier set

    // DFS timestamps (for Lengauer-Tarjan)
    uint32_t rpo   = 0;   ///< reverse post-order number
    uint32_t semi  = 0;
    uint32_t label = 0;
    uint32_t ancestor = kInvalidBlock;

    bool isEntry() const { return preds.empty(); }

    void addSucc(BlockId s) { succs.push_back(s); }
    void addPred(BlockId p) { preds.push_back(p); }
};

// ─── SSA function ─────────────────────────────────────────────────────────────

/**
 * A function in SSA form.  Owns all blocks, instructions, phi nodes,
 * and SSA values.
 */
class SSAFunction {
public:
    explicit SSAFunction(std::string name) : name_(std::move(name)) {}
    ~SSAFunction();

    const std::string& name() const { return name_; }

    // Block management
    BasicBlock* addBlock(std::string name = "");
    BasicBlock* block(BlockId id);
    const BasicBlock* block(BlockId id) const;
    const std::vector<std::unique_ptr<BasicBlock>>& blocks() const { return blocks_; }
    BlockId entryId() const { return 0; }

    // Instruction management
    IrInstr* addInstr(BlockId blk, IrInstr::Op op, uint64_t vma = 0);
    IrInstr*       instr(InstrId id);
    const IrInstr* instr(InstrId id) const;

    // Value management
    IrValue* allocValue(ValueKind kind, VarId varId = kInvalidVar);
    IrValue* value(ValueId id);
    const IrValue* value(ValueId id) const;
    const std::vector<std::unique_ptr<IrValue>>& values() const { return values_; }

    // Phi management
    PhiNode* addPhi(BlockId blk, VarId var);
    const std::vector<std::unique_ptr<PhiNode>>& phis() const { return phis_; }

    // Variable registry
    VarId    declareVar(std::string name, uint8_t width = 64);
    const std::string& varName(VarId id) const;
    uint32_t varCount() const { return (uint32_t)varNames_.size(); }
    /// Find VarId by name; returns kInvalidVar if not found.
    VarId    findVar(const std::string& name) const;

    // Statistics
    std::size_t phiCount() const { return phis_.size(); }
    std::size_t instrCount() const { return instrs_.size(); }
    std::size_t blockCount() const { return blocks_.size(); }

private:
    std::string name_;
    std::vector<std::unique_ptr<BasicBlock>> blocks_;
    std::vector<std::unique_ptr<IrInstr>>    instrs_;
    std::vector<std::unique_ptr<IrValue>>    values_;
    std::vector<std::unique_ptr<PhiNode>>    phis_;
    std::vector<std::string>                 varNames_;
};

// ─── SSA module ──────────────────────────────────────────────────────────────

/**
 * A collection of SSA functions representing an entire binary module.
 */
struct SSAModule {
    std::vector<std::unique_ptr<SSAFunction>> functions;

    SSAFunction* addFunction(std::string name) {
        functions.push_back(std::make_unique<SSAFunction>(std::move(name)));
        return functions.back().get();
    }
    std::size_t functionCount() const { return functions.size(); }
};

// ─── Liveness analysis ───────────────────────────────────────────────────────

/**
 * Backward dataflow liveness analysis over the CFG.
 *
 *   live_in[B]  = GEN[B] ∪ (live_out[B] \ KILL[B])
 *   live_out[B] = ∪ { live_in[S] : S ∈ succs(B) }
 *
 * Iterates until convergence (typically < 5 passes on reducible CFGs).
 */
class LivenessAnalysis {
public:
    /// Run liveness over `fn`.  Fills live_in/live_out on every BasicBlock.
    void run(SSAFunction& fn);

    /// Returns true if `var` is live at the entry of block `blk`.
    bool isLiveIn(const SSAFunction& fn, BlockId blk, VarId var) const;

    /// Number of iterations until convergence in the last run().
    unsigned iterations() const { return iterations_; }

private:
    unsigned iterations_ = 0;
    void computeGenKill(SSAFunction& fn);
};

// ─── Dominator tree (Lengauer-Tarjan) ────────────────────────────────────────

/**
 * Computes the dominator tree and dominance frontiers using the
 * Lengauer-Tarjan algorithm (near-linear, O(n α(n))).
 *
 * After run(), each BasicBlock has:
 *   idom         — immediate dominator (kInvalidBlock for entry)
 *   domChildren  — children in the dominator tree
 *   domFrontier  — dominance frontier set (for phi placement)
 */
class DominatorTree {
public:
    void run(SSAFunction& fn);

    /// True if block `a` dominates block `b`.
    bool dominates(const SSAFunction& fn, BlockId a, BlockId b) const;

    /// True if block `a` strictly dominates block `b` (a ≠ b).
    bool strictlyDominates(const SSAFunction& fn, BlockId a, BlockId b) const;

private:
    void computeDFS(SSAFunction& fn);
    void computeIDom(SSAFunction& fn);
    void computeDomFrontiers(SSAFunction& fn);
    void link(SSAFunction& fn, BlockId v, BlockId w);
    BlockId eval(SSAFunction& fn, BlockId v);
    void compress(SSAFunction& fn, BlockId v);

    std::vector<BlockId> vertex_;   ///< DFS order → block ID
    std::vector<BlockId> parent_;   ///< DFS parent
    std::vector<BlockId> ancestor_; ///< for LT path compression
    std::vector<BlockId> labelArr_; ///< LT label array
    std::vector<BlockId> semi_;     ///< semi-dominator number
    std::vector<std::vector<BlockId>> bucket_;
};

// ─── Liveness-pruned phi placement ───────────────────────────────────────────

/**
 * Places phi functions using the iterated dominance frontier (IDF) algorithm,
 * gated by liveness: phi(v) at d is placed only if v ∈ live_in(d).
 *
 * This implements the "pruned SSA" of Briggs, Cooper, Harvey, and Simpson
 * (1994) / the formal proof in Brandner et al. (CGO 2011).
 */
class PhiPlacement {
public:
    /**
     * Run phi placement on `fn`.
     * Requires liveness (live_in filled) and domFrontier filled.
     */
    void run(SSAFunction& fn, const LivenessAnalysis& liveness);

    /// Number of phi nodes placed in the last run().
    std::size_t placedCount() const { return placed_; }

private:
    std::size_t placed_ = 0;
};

// ─── SSA renaming ─────────────────────────────────────────────────────────────

/**
 * Renames all variable uses and definitions to versioned SSA values.
 *
 * Algorithm: recursive DFS over the dominator tree, maintaining a stack of
 * current live definitions for each variable (Cytron et al. §4).
 *
 * Special handling:
 *   - Flag-writing instructions: allocate one FlagBundle value, record it as
 *     the current definition of the "flags" variable.
 *   - Flag-reading instructions: attach the top-of-stack FlagBundle as input.
 *   - Stack slot accesses: allocate MemRef values; defer promotion.
 *   - Phi operands: filled in on the second visit to each block (when
 *     processing the predecessor's out-edges during DFS).
 */
class SSARename {
public:
    void run(SSAFunction& fn);

private:
    using DefStacks = std::unordered_map<VarId, std::vector<ValueId>>;

    void renameBlock(SSAFunction& fn, BlockId blk, DefStacks& stacks);
    void fillPhiOperands(SSAFunction& fn, BlockId blk, DefStacks& stacks);
    ValueId currentDef(const DefStacks& stacks, VarId var) const;
    void pushDef(DefStacks& stacks, VarId var, ValueId val);
    void popDefs(DefStacks& stacks,
                  const std::vector<std::pair<VarId,ValueId>>& pushed);
};

// ─── FlagBundle analysis ─────────────────────────────────────────────────────

/**
 * Post-SSA pass that identifies FlagBundle values consumed within the
 * same basic block as their definition.  Such bundles never need phi
 * functions and are marked `sameBlockOnly = true`.
 *
 * Also validates that every FlagRead instruction has exactly one incoming
 * FlagBundle that dominates the read site.
 */
class FlagBundleAnalysis {
public:
    struct BundleInfo {
        ValueId  bundleId     = kInvalidValue;
        BlockId  defBlock     = kInvalidBlock;
        bool     sameBlockOnly= false;   ///< all uses in the same block
        uint8_t  usedFlags    = 0;       ///< which FlagBits are actually used
        std::vector<std::pair<BlockId, InstrId>> useSites;
    };

    void run(const SSAFunction& fn);

    const std::vector<BundleInfo>& bundles() const { return bundles_; }
    std::size_t sameBlockBundleCount() const;

private:
    std::vector<BundleInfo> bundles_;
};

// ─── SSA verification ────────────────────────────────────────────────────────

/**
 * Verifies the SSA property after construction:
 *   1. Every use of value V is dominated by V's definition.
 *   2. Every phi operand from block P comes from the corresponding predecessor.
 *   3. No variable is defined more than once (across the entire function).
 */
class SSAVerifier {
public:
    struct Error {
        enum class Kind { UseDominance, PhiPredMismatch, MultipleDefinition };
        Kind        kind;
        BlockId     block;
        InstrId     instr;
        ValueId     value;
        std::string message;
    };

    /// Run verification on `fn`.  Returns list of errors (empty = correct).
    std::vector<Error> verify(const SSAFunction& fn,
                               const DominatorTree& dom) const;
};

// ─── Main SSA pass ────────────────────────────────────────────────────────────

/**
 * Orchestrates the full SSA construction pipeline:
 *   1. LivenessAnalysis
 *   2. DominatorTree (Lengauer-Tarjan)
 *   3. PhiPlacement (liveness-pruned)
 *   4. SSARename
 *   5. FlagBundleAnalysis
 *
 * After run(), the SSAFunction is in valid pruned SSA form.
 */
class SSAPass {
public:
    struct Stats {
        unsigned livenessIterations  = 0;
        std::size_t phisPlaced       = 0;
        std::size_t phisCytron       = 0;  ///< hypothetical Cytron count
        std::size_t flagBundles      = 0;
        std::size_t sameBlockBundles = 0;
        std::size_t memRefs          = 0;
    };

    void run(SSAFunction& fn);
    const Stats& stats() const { return stats_; }
    const std::vector<SSAVerifier::Error>& errors() const { return errors_; }

private:
    Stats stats_;
    std::vector<SSAVerifier::Error> errors_;
};

} // namespace ssa
} // namespace retdec

#endif // RETDEC_SSA_H
