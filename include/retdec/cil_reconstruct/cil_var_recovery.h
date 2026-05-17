/**
 * @file include/retdec/cil_reconstruct/cil_var_recovery.h
 * @brief CIL variable recovery — Phase 3 of the CIL reconstruction pipeline.
 *
 * ## Overview
 *
 * After stack simulation, every instruction knows the type and provenance
 * of each operand.  This phase converts the stack machine into a named
 * variable model:
 *
 *   1. **Local variable naming**: `stloc N` / `ldloc N` pairs are assigned
 *      a stable name (from PDB debug info if available, otherwise `locN`).
 *
 *   2. **Parameter naming**: `starg N` / `ldarg N` pairs use the parameter
 *      names from the method signature (or `argN` as fallback).
 *
 *   3. **Slot coalescing**: if the same stack slot is stored and reloaded
 *      via a single `stloc`/`ldloc` pair, the load is inlined back into the
 *      consuming expression (eliminating the temporary variable).
 *
 *   4. **Multiple-write detection**: locals written more than once are kept
 *      as real variables (assignments cannot be inlined).
 *
 *   5. **Type refinement**: the type of each local/parameter is set to the
 *      most specific compatible type seen across all stores.
 *
 * ## CilLocalVar — recovered variable
 *
 * Each variable in the recovered model has:
 *   - A stable name (from PDB or generated)
 *   - A refined BcType
 *   - A list of definition sites (stloc/starg instruction indices)
 *   - A list of use sites (ldloc/ldarg instruction indices)
 *   - A flag: `isInlineable` (single-def single-use in same block)
 *
 * ## CilParam — recovered parameter
 *
 * Similarly, each parameter has:
 *   - A name (from method descriptor or PDB)
 *   - A BcType
 *   - Whether it's passed by reference (BYREF)
 *   - Whether it's an `out` parameter (no incoming value)
 *
 * ## CilRecoveredMethod
 *
 * Wraps the BcMethod with the recovered variable/parameter information
 * and a vector of `CilStmt` — the statement-level representation used
 * by the C# emitter.
 */

#ifndef RETDEC_CIL_RECONSTRUCT_CIL_VAR_RECOVERY_H
#define RETDEC_CIL_RECONSTRUCT_CIL_VAR_RECOVERY_H

#include "retdec/bc_module/bc_cfg.h"
#include "retdec/bc_module/bc_module.h"
#include "retdec/cil_reconstruct/cil_stack_sim.h"

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace retdec {
namespace cil_reconstruct {

using namespace bc_module;

// ─── CilLocalVar ──────────────────────────────────────────────────────────────

struct CilLocalVar {
    uint32_t    index       = 0;        ///< Index in LocalVarSig
    std::string name;                   ///< Recovered or generated name
    BcType      type;                   ///< Refined type

    bool        isInlineable= false;    ///< Can be inlined at point of use
    bool        isTemp      = false;    ///< Compiler-generated temp
    bool        isPinned    = false;    ///< `fixed` / pinned local

    // SSA-like def/use tracking
    std::vector<uint32_t> defBlocks;   ///< Block IDs where this local is written
    std::vector<uint32_t> useBlocks;   ///< Block IDs where this local is read
    uint32_t              defCount = 0; ///< Total number of stores
    uint32_t              useCount = 0; ///< Total number of loads
};

// ─── CilParam ─────────────────────────────────────────────────────────────────

struct CilParam {
    uint32_t    index    = 0;
    std::string name;
    BcType      type;
    bool        byRef    = false;   ///< ref / out parameter
    bool        isOut    = false;   ///< [Out] attribute or unconditionally written before read
    bool        isParams = false;   ///< params keyword (vararg array)
};

// ─── CilStmt — statement IR ──────────────────────────────────────────────────

/**
 * @brief A statement in the recovered CIL model.
 *
 * Statements are simpler than BcInstructions — they operate on named
 * variables and carry complete expression trees.  They map 1:1 to C#
 * statement syntax.
 */
enum class StmtKind {
    // Variable operations
    LocalDecl,       ///< type name [= expr];
    Assign,          ///< target = expr;
    CompoundAssign,  ///< target op= expr;  (e.g., x += 1)

    // Control flow
    If,              ///< if (cond) goto/block
    Goto,            ///< goto label
    Label,           ///< label:
    Return,          ///< return [expr];
    Throw,           ///< throw expr;
    Rethrow,         ///< rethrow;
    EndFinally,      ///< (internal: end of finally block)
    EndFilter,       ///< (internal: endfilter expr)
    Leave,           ///< (internal: leave target)

    // Expression statements
    ExprStmt,        ///< expr;  (void call, side-effecting expression)

    // Try/catch/finally (structured)
    Try,             ///< try { ... }
    Catch,           ///< catch (Type e) { ... }
    Filter,          ///< catch when (filter_expr) { ... }
    Finally,         ///< finally { ... }
    Fault,           ///< fault { ... }

    // High-level patterns (added by pattern detection)
    ForEach,         ///< foreach (var x in collection) { ... }
    Using,           ///< using (var x = ...) { ... }
    Lock,            ///< lock (expr) { ... }
    YieldReturn,     ///< yield return expr;
    YieldBreak,      ///< yield break;
    AwaitExpr,       ///< await expr  (used as sub-expression within Assign/ExprStmt)

    // Fixed/unsafe
    Fixed,           ///< fixed (T* ptr = ...) { ... }
    Stackalloc,      ///< T* ptr = stackalloc T[n];

    // Switch
    Switch,          ///< switch (expr) { case ...: ... }
};

struct CilStmt {
    StmtKind   kind   = StmtKind::ExprStmt;
    CilExprPtr expr;           ///< Primary expression (condition, RHS, thrown value…)
    CilExprPtr target;         ///< LHS for assignments
    BcType     declType;       ///< For LocalDecl
    std::string labelName;     ///< For Goto/Label
    uint32_t   blockRef = 0;   ///< For Goto: target block ID
    uint32_t   line     = 0;   ///< Source line (from PDB if available)

    // For structured try/catch: child statements
    std::vector<CilStmt> tryBody;
    struct CatchClause {
        BcType      catchType;
        std::string varName;
        bool        isFilter = false;
        CilExprPtr  filterExpr;
        std::vector<CilStmt> body;
    };
    std::vector<CatchClause> catches;
    std::vector<CilStmt> finallyBody;
    std::vector<CilStmt> faultBody;

    // For switch
    struct SwitchCase {
        std::vector<CilExprPtr> values;  // empty = default
        std::vector<CilStmt>    body;
    };
    std::vector<SwitchCase> cases;

    // For foreach/using/lock/fixed
    std::vector<CilStmt> loopBody;
    std::string          iterVarName;
    BcType               iterVarType;
};

// ─── CilRecoveredBlock ────────────────────────────────────────────────────────

struct CilRecoveredBlock {
    uint32_t              id;
    std::vector<CilStmt>  stmts;
    std::vector<uint32_t> succs;

    bool isEHEntry = false;  ///< This block is the entry of an EH handler
    bool isLoop    = false;
};

// ─── CilRecoveredMethod ───────────────────────────────────────────────────────

/**
 * @brief The fully recovered variable-based representation of a CIL method.
 *
 * This is the output of the variable recovery phase and the input to
 * the C# emitter.
 */
struct CilRecoveredMethod {
    const BcMethod*               method = nullptr;

    // Recovered parameters and locals
    std::vector<CilParam>         params;
    std::vector<CilLocalVar>      locals;

    // Recovered statement blocks
    std::vector<CilRecoveredBlock> blocks;

    // Structured body (top-level statements, after structuring)
    std::vector<CilStmt>          body;

    // High-level pattern flags (set by pattern detector)
    bool isAsync         = false;   ///< Contains async/await
    bool isIterator      = false;   ///< yield return/break
    bool hasUnsafe       = false;   ///< Fixed/stackalloc/unsafe pointer ops
    bool hasLinq         = false;
    bool isPropertyGetter = false;
    bool isPropertySetter = false;
    bool isEventAdd       = false;
    bool isEventRemove    = false;
    bool hasPatternMatch  = false;  ///< Contains C# pattern-matching constructs

    // Error tracking
    bool parseError = false;
    std::string errorMsg;
};

// ─── CilVarRecovery ───────────────────────────────────────────────────────────

/**
 * @brief Converts BcCFG + stack simulation results into a CilRecoveredMethod.
 *
 * Phases performed:
 *   1. Build local variable table from LocalVarSig + stack sim types.
 *   2. Build parameter table from method signature + param names.
 *   3. Coalesce stloc/ldloc pairs (inline or assign).
 *   4. Convert each block's BcInstructions → CilStmt list.
 *   5. Propagate PDB names where available.
 */
class CilVarRecovery {
public:
    struct Options {
        bool inlineTemps       = true;   ///< Inline single-def single-use locals
        bool prefixLocals      = false;  ///< Force "loc" prefix even for PDB-named vars
        bool emitVarDecls      = true;   ///< Emit explicit local var declarations
        bool mergeInitAssign   = true;   ///< Merge LocalDecl + Assign → LocalDecl with init
    };
    static Options defaultOptions() noexcept { return {}; }

    explicit CilVarRecovery(const Options& opts = defaultOptions());

    /**
     * @brief Recover variables from a simulated method.
     *
     * @param cfg    The BcCFG (from CIL lifter, with BcCFG structure).
     * @param method The original BcMethod (types, names, param info).
     * @param sim    The completed stack simulation.
     * @return       The recovered method representation.
     */
    CilRecoveredMethod recover(const BcCFG& cfg,
                                const BcMethod& method,
                                const CilStackSimulator& sim) const;

private:
    Options opts_;

    // Build initial local var table
    std::vector<CilLocalVar> buildLocals(
        const BcMethod& method,
        const CilStackSimulator& sim,
        const BcCFG& cfg) const;

    // Build parameter table
    std::vector<CilParam> buildParams(const BcMethod& method) const;

    // Convert one block's instructions to CilStmts
    std::vector<CilStmt> convertBlock(
        const BcBasicBlock& block,
        const CilStackSimulator& sim,
        std::vector<CilLocalVar>& locals,
        const std::vector<CilParam>& params) const;

    // Convert one instruction to zero or more CilStmts
    std::vector<CilStmt> convertInsn(
        const BcInstruction& insn,
        uint32_t blockId,
        uint32_t insnIdx,
        const CilStackSimulator& sim,
        std::vector<CilLocalVar>& locals,
        const std::vector<CilParam>& params,
        StackState& workStack) const;

    // Compute inlineability for all locals
    void computeInlineability(std::vector<CilLocalVar>& locals,
                               const BcCFG& cfg) const;

    // Expression builder for LHS of stloc/starg
    CilExprPtr buildLocalLhs(uint32_t localIdx,
                              const std::vector<CilLocalVar>& locals) const;
    CilExprPtr buildArgLhs(uint32_t argIdx,
                            const std::vector<CilParam>& params) const;
};

} // namespace cil_reconstruct
} // namespace retdec

#endif // RETDEC_CIL_RECONSTRUCT_CIL_VAR_RECOVERY_H
